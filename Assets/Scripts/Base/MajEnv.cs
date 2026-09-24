using System.IO;
using UnityEngine;

namespace MajdataViewX.Base
{
    public static class MajEnv
    {
#if UNITY_EDITOR
        // 编辑器下，指向项目根目录（Assets 的上一级）
        public static string MajBase = new DirectoryInfo(Application.dataPath).Parent!.FullName;
#elif UNITY_STANDALONE_OSX
        // macOS: Application.dataPath is <MajdataViewX.app>/Contents. Use the
        // folder holding the bundle, where the editor lives, so both sides
        // share majdata_time.dat and nothing writes inside the signed bundle.
        public static string MajBase = new DirectoryInfo(Application.dataPath).Parent!.Parent!.FullName;
#else
        // 打包后，Application.dataPath 的上一级在 Windows 下是 exe 目录
        // 但为了兼顾 Mac 等平台，用 AppContext 或者是 dataPath 的物理同级更安全
        public static string MajBase => System.AppDomain.CurrentDomain.BaseDirectory;
#endif

        public static string GetPath(string relativePath) =>
            Path.Combine(MajBase, relativePath);

        public static string MmfAudioTimePath => GetPath("majdata_time.dat");
    }
}