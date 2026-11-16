# Code shrinking is disabled (minifyEnabled false). If it is ever enabled, everything
# native code reaches through JNI by name must be kept:
-keep class org.libsdl.app.** { *; }
-keep class org.shadowrecomp.android.GameActivity { *; }
