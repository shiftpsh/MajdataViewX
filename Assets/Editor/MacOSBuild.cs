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
    ///         [-il2cppConfig Release]
    ///
    /// -il2cppConfig overrides the project's IL2CPP configuration (Master)
    /// for this build only; Release links far faster for local testing.
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

            var projectConfig = PlayerSettings.GetIl2CppCompilerConfiguration(NamedBuildTarget.Standalone);
            var config = GetArgument("-il2cppConfig");
            if (config != null)
                PlayerSettings.SetIl2CppCompilerConfiguration(NamedBuildTarget.Standalone,
                    Enum.Parse<Il2CppCompilerConfiguration>(config, true));

            BuildReport report;
            try
            {
                report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
                {
                    scenes = EditorBuildSettings.scenes
                        .Where(scene => scene.enabled)
                        .Select(scene => scene.path)
                        .ToArray(),
                    locationPathName = output,
                    target = BuildTarget.StandaloneOSX,
                    options = BuildOptions.None
                });
            }
            finally
            {
                // Keep ProjectSettings.asset (shared with Windows) unchanged.
                PlayerSettings.SetIl2CppCompilerConfiguration(NamedBuildTarget.Standalone, projectConfig);
            }

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
