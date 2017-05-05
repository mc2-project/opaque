#include <jni.h>

#ifndef _Included_SP
#define _Included_SP
#ifdef __cplusplus
extern "C" {
#endif
  JNIEXPORT void JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_SPProcMsg0(
    JNIEnv *, jobject, jbyteArray);

  JNIEXPORT jbyteArray JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_SPProcMsg1(
    JNIEnv *, jobject, jbyteArray);

  JNIEXPORT jbyteArray JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_SPProcMsg3(
    JNIEnv *, jobject, jbyteArray);

  JNIEXPORT void JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_LoadKeys(
    JNIEnv *, jobject);

  JNIEXPORT jbyteArray JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_Encrypt(
    JNIEnv *, jobject, jbyteArray);

  JNIEXPORT jbyteArray JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_Decrypt(
    JNIEnv *, jobject, jbyteArray);

  JNIEXPORT jbyteArray JNICALL Java_edu_berkeley_cs_rise_opaque_execution_SP_Decrypt(
    JNIEnv *, jobject, jbyteArray);

#ifdef __cplusplus
}
#endif
#endif
