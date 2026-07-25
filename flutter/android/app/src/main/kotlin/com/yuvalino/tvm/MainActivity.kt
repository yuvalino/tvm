package com.yuvalino.tvm

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

// Exposes the two filesystem locations Dart can't derive on its own — the
// android parallel of the iOS resolvedExecutable/NSHomeDirectory logic in
// worker_controller.dart:
//   nativeLibraryDir : where the packaged .so's (libtvm.so + sidecars) are
//                      extracted; goes on TVM's PATH so launchers resolve them.
//   filesDir         : the app's writable dir; used as HOME and to stage the
//                      initrd/initrc assets (android assets are not real files).
class MainActivity : FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "com.yuvalino.tvm/paths")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "nativeLibraryDir" -> result.success(applicationInfo.nativeLibraryDir)
                    "filesDir" -> result.success(filesDir.absolutePath)
                    else -> result.notImplemented()
                }
            }
    }
}
