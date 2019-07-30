// TODO needs to be replaced with cross platform shimming approach

#include <Zydis/Zydis.h>
#include <bson.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "draconity.h"

#include "codehook.h"
#include "symbolicator.h"


#ifndef streq
#define streq(a, b) !strcmp(a, b)
#endif

// #define DEBUG


#include "api.h" // this defines the function pointers


int draconity_set_param(const char *key, const char *value) {
    if (!_engine) return -1;
    void *param = _DSXEngine_GetParam(_engine, key);
    int ret = _DSXEngine_SetStringValue(_engine, param, value);
    _DSXEngine_DestroyParam(_engine, param);
    return ret;
}

#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
static char *homedir() {
    // This should return "<userdir>/AppData/Roaming" on Windows 7+.
    // E.g: "C:/Users/Michael/AppData/Roaming"
    char *home = getenv("APPDATA");
    if (home) {
        return home;
    } else {
        return NULL;
    }
}
#elif defined(__APPLE__)
static char *homedir() {
    char *home = getenv("HOME");
    if (home) {
        return strdup(home);
    } else {
        struct passwd pw, *pwp;
        char buf[1024];
        if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &pwp) == 0) {
            return strdup(pwp->pw_dir);
        }
        return NULL;
    }
}
#else
#error "Unsupported OS for homedir"
#endif // defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)

typedef struct {
    char *timeout;
    char *timeout_incomplete;
} config;

static bool prevent_wake = false;

void draconity_set_default_params() {
    config config = {
        .timeout = strdup("80"),
        .timeout_incomplete = strdup("500"),
    };

    draconity_set_param("DwTimeOutComplete", config.timeout);
    draconity_set_param("DwTimeOutIncomplete", config.timeout_incomplete);
    free(config.timeout);
    free(config.timeout_incomplete);
    memset(&config, 0, sizeof(config));

    draconity_set_param("DemonThreadPhraseFinishWait", "0");
    // draconity_set_param("TwoPassSkipSecondPass", "1");
    // draconity_set_param("ReturnPhonemes", "1");
    // draconity_set_param("ReturnNoise", "1");
    // draconity_set_param("ReturnPauseFillers", "1");
    // draconity_set_param("Pass2DefaultSpeed", "1");
    draconity_set_param("NumWordsAvailable", "10000");
    // draconity_set_param("MinPartialUpdateCFGTime", "1");
    // draconity_set_param("MinPartialUpdateTime", "1");
    // draconity_set_param("LiveMicMinStopFrames", "1");
    // draconity_set_param("SkipLettersVocInVocLookup", "1");
    draconity_set_param("UseParallelRecognizers", "1");
    // draconity_set_param("UsePitchTracking", "1");
    draconity_set_param("Pass1A_DurationThresh_ms", "50");
    // draconity_set_param("ComputeSpeed", "10");
    // draconity_set_param("DisableWatchdog", "1");
    // draconity_set_param("DoBWPlus", "1");
    draconity_set_param("ExtraDictationWords", "10000");
    draconity_set_param("MaxCFGWords", "20000");
    draconity_set_param("MaxPronGuessedWords", "20000");
    draconity_set_param("PhraseHypothesisCallbackThread", "1");
    // CrashOnSDAPIError
}

static void engine_setup(drg_engine *engine) {
    static unsigned int cb_key = 0;
    int ret = _DSXEngine_RegisterAttribChangedCallback(engine, draconity_attrib_changed, NULL, &cb_key);
    if (ret) draconity_logf("error adding attribute callback: %d", ret);

    ret = _DSXEngine_RegisterMimicDoneCallback(engine, draconity_mimic_done, NULL, &cb_key);
    if (ret) draconity_logf("error adding mimic done callback: %d", ret);

    ret = _DSXEngine_RegisterPausedCallback(engine, draconity_paused, (void*)"paused?", NULL, &cb_key);
    if (ret) draconity_logf("error adding paused callback: %d", ret);

    ret = _DSXEngine_SetBeginPhraseCallback(engine, draconity_phrase_begin, NULL, &cb_key);
    if (ret) draconity_logf("error setting phrase begin callback: %d", ret);

    draconity_set_default_params();

    printf("[+] status: start\n");
    fflush(stdout);
    draconity_publish("status", BCON_NEW("cmd", BCON_UTF8("start")));
}

static void engine_acquire(drg_engine *engine, bool early) {
    if (!_engine) {
        printf("[+] engine acquired\n");
        _engine = engine;
        engine_setup(engine);
    }
    if (!early && !draconity->ready) {
        if (_DSXEngine_GetCurrentSpeaker(_engine)) {
            draconity_ready();
        }
    }
}

static int DSXEngine_SetBeginPhraseCallback() {
    draconity_logf("warning: called stubbed SetBeginPhraseCallback()\n");
    return 0;
}

static int DSXEngine_SetEndPhraseCallback() {
    draconity_logf("warning: called stubbed SetEndPhraseCallback()\n");
    return 0;
}

static int DSXFileSystem_PreferenceSetValue(drg_filesystem *fs, char *a, char *b, char *c, char *d) {
    // printf("DSXFileSystem_PreferenceSetValue(%p, %s, %s, %s, %s);\n", fs, a, b, c, d);
    return _DSXFileSystem_PreferenceSetValue(fs, a, b, c, d);
}

// track which dragon grammars are active, so "dragon" pseudogrammar can activate them
int DSXGrammar_Activate(drg_grammar *grammar, uint64_t unk1, bool unk2, const char *main_rule) {
    draconity->dragon_lock.lock();
    ForeignGrammar *foreign_grammar = new ForeignGrammar(grammar, unk1, unk2, main_rule);
    draconity->dragon_grammars.push_back(foreign_grammar);
    int ret = 0;
    if (draconity->dragon_enabled) {
        ret = _DSXGrammar_Activate(grammar, unk1, unk2, main_rule);
    }
    draconity->dragon_lock.unlock();
    return ret;
}

int DSXGrammar_Deactivate(drg_grammar *grammar, uint64_t unk1, const char *main_rule) {
    draconity->dragon_lock.lock();
    // Remove the grammar from the draconity's internal map (if it exists).
    ForeignGrammar *grammar_to_remove = NULL;
    for (ForeignGrammar *foreign_grammar : draconity->dragon_grammars) {
        if (foreign_grammar->matches(grammar, main_rule)) {
            // Can't remove within the for loop - store the grammar, then break
            // and remove it.
            grammar_to_remove = foreign_grammar;
            break;
        }
    }
    if (grammar_to_remove) {
        draconity->dragon_grammars.remove(grammar_to_remove);
        delete grammar_to_remove;
    }

    // Now Draconity's record of the grammar has been removed, it can be
    // disabled.
    int ret = 0;
    if (draconity->dragon_enabled) {
        ret = _DSXGrammar_Deactivate(grammar, unk1, main_rule);
    }
    draconity->dragon_lock.unlock();
    return ret;
}

int DSXGrammar_SetList(drg_grammar *grammar, const char *name, dsx_dataptr *data) {
    draconity->dragon_lock.lock();
    int ret = 0;
    if (draconity->dragon_enabled) {
        ret = _DSXGrammar_SetList(grammar, name, data);
    }
    draconity->dragon_lock.unlock();
    return ret;
}

/*
int DSXGrammar_RegisterEndPhraseCallback(drg_grammar *grammar, void *cb, void *user, unsigned int *key) {
    *key = 0;
    return 0;
    // return _DSXGrammar_RegisterEndPhraseCallback(grammar, cb, user, key);
}

int DSXGrammar_RegisterBeginPhraseCallback(drg_grammar *grammar, void *cb, void *user, unsigned int *key) {
    *key = 0;
    return 0;
    // return _DSXGrammar_RegisterBeginPhraseCallback(grammar, cb, user, key);
}

int DSXGrammar_RegisterPhraseHypothesisCallback(drg_grammar *grammar, void *cb, void *user, unsigned int *key) {
    *key = 0;
    return 0;
    // return _DSXGrammar_RegisterPhraseHypothesisCallback(grammar, cb, user, key);
}
*/


drg_engine* (*orig_DSXEngine_New)();
static drg_engine* DSXEngine_New() {
    drg_engine *engine = orig_DSXEngine_New();
    draconity_logf("DSXEngine_New() = %p", engine);
    engine_acquire(engine, true);
    return _engine;
}

int (*orig_DSXEngine_Create)(char *s, uint64_t val, drg_engine **engine);
static int DSXEngine_Create(char *s, uint64_t val, drg_engine **engine) {
    int ret = orig_DSXEngine_Create(s, val, engine);
    draconity_logf("DSXEngine_Create(%s, %llu, &%p) = %d", s, val, engine, ret);
    engine_acquire(*engine, true);
    return ret;
}

int (*orig_DSXEngine_GetMicState)(drg_engine *engine, int64_t *state);
static int DSXEngine_GetMicState(drg_engine *engine, int64_t *state) {
    engine_acquire(engine, false);
    return orig_DSXEngine_GetMicState(engine, state);
}

// TODO: I've manually specified various types here. Revert them to void?
int (*orig_DSXEngine_LoadGrammar)(drg_engine* engine,
                                  int format,
                                  void *data,
                                  void **grammar);
static int DSXEngine_LoadGrammar(drg_engine *engine,
                                 int format,
                                 void *data,
                                 void **grammar) {
    engine_acquire(engine, false);
    return orig_DSXEngine_LoadGrammar(engine, format, data, grammar);
}


/* Helper function to create a typed CodeHook and automatically infer F

   F doesn't need to be explicitly provided. It will be inferred from the type
   of `target` & `*original` (target and *original must both have the same
   type).

   Please note, since this creates a new CodeHook, it will need to be manually
   deleted when it is no longer needed.
*/
template <typename F> CodeHook<F> *makeCodeHook(std::string name, F target, F *original) {
	return new CodeHook<F>(name, target, original);
}

static std::list<CodeHookBase*> dragon_hooks = {
    makeCodeHook("DSXEngine_New", DSXEngine_New, &orig_DSXEngine_New),
    makeCodeHook("DSXEngine_Create", DSXEngine_Create, &orig_DSXEngine_Create),
    makeCodeHook("DSXEngine_GetMicState", DSXEngine_GetMicState, &orig_DSXEngine_GetMicState),
    makeCodeHook("DSXEngine_LoadGrammar", DSXEngine_LoadGrammar, &orig_DSXEngine_LoadGrammar),
};

/* Helper function to create a typed SymbolLoad and automatically infer F

   F will be inferred by the type of the value pointed to by `ptr`.

   Please note, since this creates a new SymbolLoad, it will need to be manually
   deleted when no longer needed.
*/
template <typename F> SymbolLoad<F> *makeSymbolLoad(std::string name, F* ptr) {
    return new SymbolLoad<F>(name, ptr);
}

static std::list<SymbolLoadBase*> server_syms {
    makeSymbolLoad("DSXEngine_Create",                            &_DSXEngine_Create),
    makeSymbolLoad("DSXEngine_New",                               &_DSXEngine_New),

    makeSymbolLoad("DSXEngine_AddWord",                           &_DSXEngine_AddWord),
    makeSymbolLoad("DSXEngine_AddTemporaryWord",                  &_DSXEngine_AddTemporaryWord),
    makeSymbolLoad("DSXEngine_DeleteWord",                        &_DSXEngine_DeleteWord),
    makeSymbolLoad("DSXEngine_ValidateWord",                      &_DSXEngine_ValidateWord),
    makeSymbolLoad("DSXEngine_EnumWords",                         &_DSXEngine_EnumWords),

    makeSymbolLoad("DSXWordEnum_GetCount",                        &_DSXWordEnum_GetCount),
    makeSymbolLoad("DSXWordEnum_Next",                            &_DSXWordEnum_Next),
    makeSymbolLoad("DSXWordEnum_End",                             &_DSXWordEnum_End),

    makeSymbolLoad("DSXEngine_GetCurrentSpeaker",                 &_DSXEngine_GetCurrentSpeaker),
    makeSymbolLoad("DSXEngine_GetMicState",                       &_DSXEngine_GetMicState),
    makeSymbolLoad("DSXEngine_LoadGrammar",                       &_DSXEngine_LoadGrammar),
    makeSymbolLoad("DSXEngine_Mimic",                             &_DSXEngine_Mimic),
    makeSymbolLoad("DSXEngine_Pause",                             &_DSXEngine_Pause),
    makeSymbolLoad("DSXEngine_RegisterAttribChangedCallback",     &_DSXEngine_RegisterAttribChangedCallback),
    makeSymbolLoad("DSXEngine_RegisterMimicDoneCallback",         &_DSXEngine_RegisterMimicDoneCallback),
    makeSymbolLoad("DSXEngine_RegisterPausedCallback",            &_DSXEngine_RegisterPausedCallback),
    makeSymbolLoad("DSXEngine_Resume",                            &_DSXEngine_Resume),
    makeSymbolLoad("DSXEngine_ResumeRecognition",                 &_DSXEngine_ResumeRecognition),
    makeSymbolLoad("DSXEngine_SetBeginPhraseCallback",            &_DSXEngine_SetBeginPhraseCallback),
    makeSymbolLoad("DSXEngine_SetEndPhraseCallback",              &_DSXEngine_SetEndPhraseCallback),

    makeSymbolLoad("DSXEngine_SetStringValue",                    &_DSXEngine_SetStringValue),
    makeSymbolLoad("DSXEngine_GetValue",                          &_DSXEngine_GetValue),
    makeSymbolLoad("DSXEngine_GetParam",                          &_DSXEngine_GetParam),
    makeSymbolLoad("DSXEngine_DestroyParam",                      &_DSXEngine_DestroyParam),

    makeSymbolLoad("DSXFileSystem_PreferenceGetValue",            &_DSXFileSystem_PreferenceGetValue),
    makeSymbolLoad("DSXFileSystem_PreferenceSetValue",            &_DSXFileSystem_PreferenceSetValue),
    makeSymbolLoad("DSXFileSystem_SetResultsDirectory",           &_DSXFileSystem_SetResultsDirectory),
    makeSymbolLoad("DSXFileSystem_SetUsersDirectory",             &_DSXFileSystem_SetUsersDirectory),
    makeSymbolLoad("DSXFileSystem_SetVocabsLocation",             &_DSXFileSystem_SetVocabsLocation),

    makeSymbolLoad("DSXGrammar_Activate",                         &_DSXGrammar_Activate),
    makeSymbolLoad("DSXGrammar_Deactivate",                       &_DSXGrammar_Deactivate),
    makeSymbolLoad("DSXGrammar_Destroy",                          &_DSXGrammar_Destroy),
    makeSymbolLoad("DSXGrammar_GetList",                          &_DSXGrammar_GetList),
    makeSymbolLoad("DSXGrammar_RegisterBeginPhraseCallback",      &_DSXGrammar_RegisterBeginPhraseCallback),
    makeSymbolLoad("DSXGrammar_RegisterEndPhraseCallback",        &_DSXGrammar_RegisterEndPhraseCallback),
    makeSymbolLoad("DSXGrammar_RegisterPhraseHypothesisCallback", &_DSXGrammar_RegisterPhraseHypothesisCallback),
    makeSymbolLoad("DSXGrammar_SetApplicationName",               &_DSXGrammar_SetApplicationName),
    makeSymbolLoad("DSXGrammar_SetApplicationName",               &_DSXGrammar_SetApplicationName),
    makeSymbolLoad("DSXGrammar_SetList",                          &_DSXGrammar_SetList),
    makeSymbolLoad("DSXGrammar_SetPriority",                      &_DSXGrammar_SetPriority),
    makeSymbolLoad("DSXGrammar_SetSpecialGrammar",                &_DSXGrammar_SetSpecialGrammar),
    makeSymbolLoad("DSXGrammar_Unregister",                       &_DSXGrammar_Unregister),

    makeSymbolLoad("DSXResult_BestPathWord",                      &_DSXResult_BestPathWord),
    makeSymbolLoad("DSXResult_GetWordNode",                       &_DSXResult_GetWordNode),
    makeSymbolLoad("DSXResult_Destroy",                           &_DSXResult_Destroy),
};

static void *draconity_install(void *_) {
    printf("[+] draconity starting\n");
    int hooked = draconity_hook_symbols(dragon_hooks, server_syms);
    if (hooked != 0) {
        printf("[!] draconity failed to hook!");
        return NULL;
    }
    draconity_init();
    return NULL;
}

__attribute__((constructor))
void cons() {
#ifdef DEBUG
    int log = open("/tmp/draconity.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
    dup2(log, 1);
    dup2(log, 2);
#endif
    draconity_install(NULL);
}
