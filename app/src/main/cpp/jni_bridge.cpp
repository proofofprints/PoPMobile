/**
 * JNI bridge — exposes the C mining engine to Kotlin/Java.
 *
 * Copyright (c) 2026 Proof of Prints
 */

#include <jni.h>
#include <android/log.h>
#include <string.h>

extern "C" {
#include "mining_engine.h"
#include "kheavyhash.h"
}

#define TAG "KASMiner-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* Global JVM reference for callbacks from native threads */
static JavaVM *g_jvm = nullptr;
static jobject g_callback_obj = nullptr;
static jmethodID g_on_share_found_method = nullptr;

/* Called when the native library is loaded */
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

/* Share found callback — bridges from C to Java */
static void native_share_callback(const char *job_id, uint64_t nonce) {
    if (!g_jvm || !g_callback_obj) return;

    JNIEnv *env;
    bool needs_detach = false;

    if (g_jvm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        needs_detach = true;
    }

    if (env && g_on_share_found_method) {
        jstring jJobId = env->NewStringUTF(job_id);
        env->CallVoidMethod(g_callback_obj, g_on_share_found_method, jJobId, (jlong)nonce);
        env->DeleteLocalRef(jJobId);
    }

    if (needs_detach) {
        g_jvm->DetachCurrentThread();
    }
}

/* ===== JNI Methods ===== */

extern "C" JNIEXPORT void JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeStart(
        JNIEnv *env, jobject thiz, jint numThreads) {
    LOGI("nativeStart called with %d threads", numThreads);
    mining_start(numThreads);
}

extern "C" JNIEXPORT void JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeStop(
        JNIEnv *env, jobject thiz) {
    LOGI("nativeStop called");
    mining_stop();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeIsRunning(
        JNIEnv *env, jobject thiz) {
    return mining_is_running() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeSetJob(
        JNIEnv *env, jobject thiz, jbyteArray headerHash, jstring jobId, jbyteArray target, jlong timestamp) {

    jbyte *header = env->GetByteArrayElements(headerHash, nullptr);
    jbyte *tgt = env->GetByteArrayElements(target, nullptr);
    const char *jid = env->GetStringUTFChars(jobId, nullptr);

    mining_set_job((const uint8_t *)header, jid, (const uint8_t *)tgt, (uint64_t)timestamp);

    env->ReleaseByteArrayElements(headerHash, header, JNI_ABORT);
    env->ReleaseByteArrayElements(target, tgt, JNI_ABORT);
    env->ReleaseStringUTFChars(jobId, jid);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeGetHashrate(
        JNIEnv *env, jobject thiz) {
    return mining_get_hashrate();
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeGetHashrateWindow(
        JNIEnv *env, jobject thiz, jdouble seconds) {
    return mining_get_hashrate_window((double)seconds);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeGetThreadHashes(
        JNIEnv *env, jobject thiz, jint threadIdx) {
    return (jlong)mining_get_thread_hashes((int)threadIdx);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeGetActiveThreads(
        JNIEnv *env, jobject thiz) {
    return (jint)mining_get_active_threads();
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeGetTotalHashes(
        JNIEnv *env, jobject thiz) {
    return (jlong)mining_get_total_hashes();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeGetSharesFound(
        JNIEnv *env, jobject thiz) {
    return (jint)mining_get_shares_found();
}

extern "C" JNIEXPORT void JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeSetShareCallback(
        JNIEnv *env, jobject thiz, jobject callback) {

    /* Clean up previous global ref */
    if (g_callback_obj) {
        env->DeleteGlobalRef(g_callback_obj);
        g_callback_obj = nullptr;
    }

    if (callback) {
        g_callback_obj = env->NewGlobalRef(callback);
        jclass cls = env->GetObjectClass(callback);
        g_on_share_found_method = env->GetMethodID(cls, "onShareFound", "(Ljava/lang/String;J)V");
        mining_set_share_callback(native_share_callback);
    } else {
        mining_set_share_callback(nullptr);
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeGetSharesRejected(
        JNIEnv *env, jobject thiz) {
    return (jint)mining_get_shares_rejected();
}

extern "C" JNIEXPORT void JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeIncrementRejected(
        JNIEnv *env, jobject thiz) {
    mining_increment_rejected();
}

extern "C" JNIEXPORT void JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeSetTargetFromDifficulty(
        JNIEnv *env, jobject thiz, jdouble difficulty, jbyteArray targetOut) {

    uint8_t target[32];
    set_target_from_difficulty(difficulty, target);

    env->SetByteArrayRegion(targetOut, 0, 32, (const jbyte *)target);
}

extern "C" JNIEXPORT void JNICALL
Java_com_proofofprints_popmobile_mining_MiningEngine_nativeSetExtranonce(
        JNIEnv *env, jobject thiz, jlong extranoncePrefix, jint extranonce2Bits) {
    LOGI("nativeSetExtranonce: prefix=0x%016llx, en2bits=%d",
         (unsigned long long)extranoncePrefix, extranonce2Bits);
    mining_set_extranonce((uint64_t)extranoncePrefix, (int)extranonce2Bits);
}
