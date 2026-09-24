// The macOS build module's editor API only exists while macOS is the active
// build target.
#if UNITY_STANDALONE_OSX
using System;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.OSXStandalone;

namespace MajdataViewX.Editor
{
    /// <summary>
    /// Builds a universal (Apple silicon + Intel) macOS player, since the
    /// default architecture is not stored in the project:
    ///
    ///   Unity -batchmode -quit -projectPath . -buildTarget OSXUniversal
    ///         -executeMethod MajdataViewX.Editor.MacOSBuild.Build
    ///         -outputPath build/MajdataViewX.app
    ///
    /// Also accepts game-ci's -customBuildPath.
    /// </summary>
    public static class MacOSBuild
    {
        public static void Build()
        {
            UserBuildSettings.architecture = OSArchitecture.x64ARM64;

            var output = GetArgument("-outputPath")
                         ?? GetArgument("-customBuildPath")
                         ?? "build/StandaloneOSX/MajdataViewX.app";
            if (!output.EndsWith(".app", StringComparison.Ordinal))
                output += ".app";

            var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
            {
                scenes = EditorBuildSettings.scenes
                    .Where(scene => scene.enabled)
                    .Select(scene => scene.path)
                    .ToArray(),
                locationPathName = output,
                target = BuildTarget.StandaloneOSX,
                options = BuildOptions.None
            });

            if (report.summary.result != BuildResult.Succeeded)
                EditorApplication.Exit(1);
        }

        private static string GetArgument(string name)
        {
            var args = Environment.GetCommandLineArgs();
            var index = Array.IndexOf(args, name);
            return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
        }
    }
}
#endif
