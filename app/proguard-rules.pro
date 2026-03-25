# Keep JNI methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep mining engine bridge
-keep class com.proofofprints.kasminer.mining.MiningEngine { *; }
