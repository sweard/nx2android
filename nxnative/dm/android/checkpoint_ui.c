#include "checkpoint_android.h"
#include "checkpoint_ui.h"
#include <stdatomic.h>

#ifdef DM_MANUAL_CHECKPOINT
enum { CHECKPOINT_WAIT_FRAME = 1, CHECKPOINT_WAIT_CHOICE, CHECKPOINT_WAIT_FIELD };
static _Atomic int checkpoint_choice_pending;
static jmethodID checkpoint_offer_method, checkpoint_status_method;
static _Atomic int awaiting_restored_frame;
JNIEXPORT void JNICALL Java_org_nx2a_GameActivity_chooseCheckpoint(JNIEnv *env, jobject activity, jint choice) {
    if (!(*env)->IsSameObject(env, activity, dm_checkpoint_activity()->clazz)) return;
#ifdef DM_FIELD_CHECKPOINT
    int expected = CHECKPOINT_WAIT_FIELD;
    if (atomic_compare_exchange_strong(&checkpoint_choice_pending, &expected, 0)) {
        atomic_store(&awaiting_restored_frame, choice == DM_CHECKPOINT_RESTORE);
        dm_checkpoint_field_choose(choice);
        dm_checkpoint_start_game();
        return;
    }
#else
    int expected = CHECKPOINT_WAIT_CHOICE;
    if (!atomic_compare_exchange_strong(&checkpoint_choice_pending, &expected, 0)) return;
    if (choice == DM_CHECKPOINT_RESTORE) {
        dm_checkpoint_restore_settings();
    }
    dm_checkpoint_consume();
#endif
}

int dm_checkpoint_ui_prepare(ANativeActivity *act, const char *files_root) {
    JNIEnv *env = act->env;
    jclass cls = (*env)->GetObjectClass(env, act->clazz);
    checkpoint_status_method = cls ? (*env)->GetMethodID(env, cls, "checkpointStatus", "(I)V") : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    checkpoint_offer_method = cls ? (*env)->GetMethodID(env, cls, "offerCheckpoint", "()V") : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!checkpoint_offer_method) {
        if (cls) (*env)->DeleteLocalRef(env, cls);
        return 0;
    }
#ifdef DM_FIELD_CHECKPOINT
    checkpoint_offer_method = (*env)->GetMethodID(env, cls, "offerRuntimeCheckpoint", "()V");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
#endif
    (*env)->DeleteLocalRef(env, cls);
    int kind = dm_checkpoint_init(act->internalDataPath, files_root);
#ifdef DM_FIELD_CHECKPOINT
    if (kind == 2) {
        atomic_store(&checkpoint_choice_pending, CHECKPOINT_WAIT_FIELD);
        if (checkpoint_offer_method) {
            (*env)->CallVoidMethod(env, act->clazz, checkpoint_offer_method);
            if (!(*env)->ExceptionCheck(env)) return 1;
        }
        atomic_store(&checkpoint_choice_pending, 0);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        dm_checkpoint_field_choose(DM_CHECKPOINT_SKIP);
    }
#else
    if (kind) atomic_store(&checkpoint_choice_pending, CHECKPOINT_WAIT_FRAME);
    else dm_checkpoint_consume();
#endif
    return 0;
}

// Notifications can originate on the save worker or the Vulkan present thread.
static int call_activity(jmethodID method, const jvalue *args) {
    ANativeActivity *act = dm_checkpoint_activity();
    if (!act || !method) return 0;
    JNIEnv *env = NULL;
    int attached = (*act->vm)->GetEnv(act->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK;
    if (attached && (*act->vm)->AttachCurrentThread(act->vm, &env, NULL) != JNI_OK) return 0;
    (*env)->CallVoidMethodA(env, act->clazz, method, args);
    int ok = !(*env)->ExceptionCheck(env);
    if (!ok) (*env)->ExceptionClear(env);
    if (attached) (*act->vm)->DetachCurrentThread(act->vm);
    return ok;
}
#endif

int dm_checkpoint_notify(int status) {
#ifdef DM_MANUAL_CHECKPOINT
    jvalue value = {.i = status};
    return call_activity(checkpoint_status_method, &value);
#else
    (void)status;
    return 0;
#endif
}

void dm_checkpoint_frame_shown(void) {
#ifdef DM_MANUAL_CHECKPOINT
    if (atomic_exchange(&awaiting_restored_frame, 0)) {
        dm_checkpoint_notify(DM_CHECKPOINT_RESTORED);
        return;
    }
    int expected = CHECKPOINT_WAIT_FRAME;
    if (!atomic_compare_exchange_strong(&checkpoint_choice_pending, &expected, CHECKPOINT_WAIT_CHOICE)) return;
    if (!call_activity(checkpoint_offer_method, NULL)) atomic_store(&checkpoint_choice_pending, 0);
#endif
}
