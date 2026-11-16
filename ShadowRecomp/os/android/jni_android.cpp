#include <os/android/jni_android.h>

#include <SDL3/SDL.h>
#include <jni.h>

#include <algorithm>
#include <cmath>

namespace os::android
{
    // RAII view of the SDL activity for the calling thread. SDL attaches the thread to the
    // VM on demand; the activity is returned as a local reference this object releases.
    struct ActivityCall
    {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        jobject activity = nullptr;
        jclass activityClass = nullptr;

        ActivityCall()
        {
            if (env == nullptr)
                return;

            activity = static_cast<jobject>(SDL_GetAndroidActivity());
            if (activity != nullptr)
                activityClass = env->GetObjectClass(activity);
        }

        ~ActivityCall()
        {
            if (env == nullptr)
                return;

            if (activityClass != nullptr)
                env->DeleteLocalRef(activityClass);

            if (activity != nullptr)
                env->DeleteLocalRef(activity);
        }

        ActivityCall(const ActivityCall&) = delete;
        ActivityCall& operator=(const ActivityCall&) = delete;

        // Resolves an instance method, clearing the NoSuchMethodError a mismatched APK
        // would otherwise leave pending on this thread.
        jmethodID Method(const char* name, const char* signature) const
        {
            if (activityClass == nullptr)
                return nullptr;

            jmethodID method = env->GetMethodID(activityClass, name, signature);
            if (env->ExceptionCheck())
            {
                env->ExceptionClear();
                return nullptr;
            }

            return method;
        }

        // True if the call just made threw; the exception is cleared either way.
        bool Failed() const
        {
            if (!env->ExceptionCheck())
                return false;

            env->ExceptionClear();
            return true;
        }
    };

    std::string LocaliseMessage(const char* resourceName, const char* fallback, const std::string& detail)
    {
        std::string result;

        ActivityCall call;
        if (jmethodID method = call.Method("getLocalizedMessage", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"))
        {
            jstring key = call.env->NewStringUTF(resourceName);
            jstring argument = call.env->NewStringUTF(detail.c_str());
            if (key != nullptr && argument != nullptr)
            {
                jstring text = static_cast<jstring>(call.env->CallObjectMethod(call.activity, method, key, argument));
                if (!call.Failed() && text != nullptr)
                {
                    if (const char* chars = call.env->GetStringUTFChars(text, nullptr))
                    {
                        result = chars;
                        call.env->ReleaseStringUTFChars(text, chars);
                    }
                }

                if (text != nullptr)
                    call.env->DeleteLocalRef(text);
            }
            else
            {
                call.Failed();
            }

            if (argument != nullptr)
                call.env->DeleteLocalRef(argument);

            if (key != nullptr)
                call.env->DeleteLocalRef(key);
        }

        if (!result.empty())
            return result;

        result = fallback != nullptr ? fallback : "";
        if (!detail.empty())
        {
            if (!result.empty())
                result += "\n";

            result += detail;
        }

        return result;
    }

    void SetInstallProgress(const char* fileName, float ratio)
    {
        ActivityCall call;
        if (jmethodID method = call.Method("setInstallProgress", "(Ljava/lang/String;I)V"))
        {
            const float clamped = std::isfinite(ratio) ? std::clamp(ratio, 0.0f, 1.0f) : 0.0f;
            jstring name = call.env->NewStringUTF(fileName != nullptr ? fileName : "");
            if (name != nullptr)
            {
                call.env->CallVoidMethod(call.activity, method, name, jint(std::lround(clamped * 1000.0f)));
                call.env->DeleteLocalRef(name);
            }

            call.Failed();
        }
    }

    void FinishInstallProgress()
    {
        ActivityCall call;
        if (jmethodID method = call.Method("finishInstallProgress", "()V"))
        {
            call.env->CallVoidMethod(call.activity, method);
            call.Failed();
        }
    }
}
