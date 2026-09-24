// Windows uses the D3D11 RenderingOut.dll (zero-copy from the native
// texture); other platforms use the portable build in Native/RenderingOut,
// fed through AsyncGPUReadback. In the Editor the host OS decides, not the
// active build target.
#if UNITY_EDITOR_WIN || (!UNITY_EDITOR && UNITY_STANDALONE_WIN)
#define RENDERINGOUT_D3D11
#endif

using Cysharp.Threading.Tasks;
using JetBrains.Annotations;
using MajdataViewX.Types.Rendering;
using System;
using System.IO;
using System.Runtime.InteropServices;
using Unity.Collections.LowLevel.Unsafe;
using UnityEngine;
using UnityEngine.UI;
using static MajdataViewX.Base.MajCtx;
#if !RENDERINGOUT_D3D11
using System.Collections.Generic;
using Unity.Collections;
using UnityEngine.Rendering;
#endif

namespace MajdataViewX.Managers
{
    public class ScreenRecorder : MonoBehaviour
    {
        private const string EncoderDllName = "RenderingOut";

        [DllImport(EncoderDllName, CallingConvention = CallingConvention.StdCall)]
        private static extern IntPtr video_encoder_create(
            int quality,
            int width,
            int height,
            int fps,
            [MarshalAs(UnmanagedType.LPStr)] string filename);

#if RENDERINGOUT_D3D11
        [DllImport(EncoderDllName, CallingConvention = CallingConvention.StdCall)]
        private static extern int video_encoder_submit_frame(
            IntPtr encoder,
            IntPtr nativeTexture);
#else
        [DllImport(EncoderDllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern int video_encoder_submit_rgba(
            IntPtr encoder,
            IntPtr pixels,
            int stride,
            int flipVertical);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void NativeLogCallback(int level, IntPtr message);

        [DllImport(EncoderDllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void renderingout_set_log_callback(NativeLogCallback callback);

        // Held in a static field so the GC never collects the native callback.
        private static readonly NativeLogCallback LogCallback = OnNativeLog;

        [AOT.MonoPInvokeCallback(typeof(NativeLogCallback))]
        private static void OnNativeLog(int level, IntPtr message)
        {
            var text = Marshal.PtrToStringUTF8(message);
            if (level == 0)
                Debug.Log(text);
            else
                Debug.LogError(text);
        }

        // Readbacks allowed in flight before the capture loop waits on the
        // oldest one; bounds both memory and output latency.
        private const int MaxPendingReadbacks = 3;
#endif

        [DllImport(EncoderDllName, CallingConvention = CallingConvention.StdCall)]
        private static extern int video_encoder_mux_audio(
            IntPtr encoder,
            IntPtr pcmData,
            int pcmLengthBytes,
            int sampleRate,
            int channels);

        [DllImport(EncoderDllName, CallingConvention = CallingConvention.StdCall)]
        private static extern void video_encoder_free(IntPtr encoder);


        public bool IsRecording { get; private set; }

        private void Awake()
        {
            _screenRecorder = this;
        }


        public async UniTask StartRecording(string maidataPath,
            int fps, ExportQuality quality, [CanBeNull] Action onStart = null)
        {
            QualitySettings.vSyncCount = 0;
            try
            {
                await CaptureScreen(maidataPath, fps, quality, onStart);
            }
            finally
            {
                QualitySettings.vSyncCount = 1;
            }
        }

        public void StopRecording()
        {
            IsRecording = false;
        }

        private async UniTask CaptureScreen(string maidataPath,
            int fps, ExportQuality quality, [CanBeNull] Action onStart = null)
        {
            if (fps <= 0)
            {
                _wsServer.Error("Encoding cannot start: Output frame rate must be greater than zero.");
                return;
            }

            const string finalName = "out.mp4";

            IsRecording = true;
            // H.264 (NV12) requires even dimensions
            // crop odd pixels so encoding always succeeds.
            var width = Screen.width & ~1;
            var height = Screen.height & ~1;
            var frameDuration = 1.0 / fps;
            var recordingElapsedTime = 0.0;
            var outputSucceeded = false;
            var captureTexture = new RenderTexture(
                width,
                height,
                0,
#if RENDERINGOUT_D3D11
                RenderTextureFormat.BGRA32)
#else
                // Metal cannot read back BGRA8; RGBA8 reads back on every API.
                RenderTextureFormat.ARGB32)
#endif
            {
                name = "Screen Recorder Capture",
                antiAliasing = 1,
                useMipMap = false,
                autoGenerateMips = false
            };
            var encoder = IntPtr.Zero;
#if !RENDERINGOUT_D3D11
            var pendingReadbacks = new Queue<AsyncGPUReadbackRequest>();
#endif

            try
            {
                if (!captureTexture.Create())
                    throw new InvalidOperationException(
                        "Could not create the screen recorder render target.");

                var outPath = Path.Combine(maidataPath, finalName);
                if (File.Exists(outPath)) File.Delete(outPath);

#if !RENDERINGOUT_D3D11
                renderingout_set_log_callback(LogCallback);
#endif
                encoder = video_encoder_create(
                    (int)quality,
                    width,
                    height,
                    fps,
                    outPath);
                if (encoder == IntPtr.Zero)
                    throw new InvalidOperationException(
                        "RenderingOut could not create the video encoder.");

                onStart?.Invoke();
                _audioManager.BeginRecordingAudio(_timeProvider.AudioTime, _timeProvider.CurrentSpeed);

                while (IsRecording)
                {
                    await UniTask.WaitForEndOfFrame(this);
                    var frameEndTime = recordingElapsedTime + frameDuration;
                    _audioManager.UpdateRecordingAudioFrame(recordingElapsedTime, frameEndTime);

                    ScreenCapture.CaptureScreenshotIntoRenderTexture(captureTexture);
#if RENDERINGOUT_D3D11
                    var nativeTexture = captureTexture.GetNativeTexturePtr();
                    if (nativeTexture == IntPtr.Zero)
                        throw new InvalidOperationException(
                            "The screen recorder render target has no native texture.");

                    var submitResult = video_encoder_submit_frame(encoder, nativeTexture);
                    if (submitResult < 0)
                        throw new InvalidOperationException(
                            $"RenderingOut failed to encode a video frame ({submitResult}).");
#else
                    // The copy is queued on the GPU now, so reusing
                    // captureTexture next frame cannot race with it.
                    pendingReadbacks.Enqueue(
                        AsyncGPUReadback.Request(captureTexture, 0, TextureFormat.RGBA32));
                    SubmitCompletedReadbacks(encoder, pendingReadbacks, width, height, MaxPendingReadbacks);
#endif

                    recordingElapsedTime = frameEndTime;
                }

#if !RENDERINGOUT_D3D11
                SubmitCompletedReadbacks(encoder, pendingReadbacks, width, height, 0);
#endif
                _audioManager.EndRecordingAudio((float)recordingElapsedTime);
                MuxRecordingAudio(encoder);
                FreeEncoder(ref encoder);
                outputSucceeded = true;
            }
            catch (Exception e)
            {
                Debug.LogException(e);
                _wsServer.Error(e);
            }
            finally
            {
                IsRecording = false;
                try
                {
                    FreeEncoder(ref encoder);
                }
                catch (Exception ex)
                {
                    outputSucceeded = false;
                    Debug.LogException(ex);
                    _wsServer.Error(ex);
                }

                _audioManager.ReleaseRecordingAudio();
                var resultPath = Path.Combine(maidataPath, finalName);
                if (outputSucceeded && File.Exists(resultPath))
                    OpenFileLocation(resultPath);

                RenderTexture.active = null;
#if !RENDERINGOUT_D3D11
                // Readbacks abandoned by an exception must finish before
                // their source texture goes away.
                AsyncGPUReadback.WaitAllRequests();
#endif
                captureTexture.Release();
                Destroy(captureTexture);
            }
        }

#if !RENDERINGOUT_D3D11
        // Submits finished readbacks in capture order, so frame timestamps
        // stay sequential. Waits on the oldest while more than maxPending
        // are still in flight.
        private static void SubmitCompletedReadbacks(IntPtr encoder,
            Queue<AsyncGPUReadbackRequest> pending, int width, int height, int maxPending)
        {
            while (pending.Count > 0)
            {
                var request = pending.Peek();
                if (!request.done)
                {
                    if (pending.Count <= maxPending)
                        return;
                    request.WaitForCompletion();
                }

                pending.Dequeue();
                if (request.hasError)
                    throw new InvalidOperationException(
                        "The GPU readback of a recorded frame failed.");
                SubmitPixels(encoder, request.GetData<byte>(), width, height);
            }
        }

        private static unsafe void SubmitPixels(IntPtr encoder,
            NativeArray<byte> pixels, int width, int height)
        {
            var stride = width * 4;
            if (pixels.Length < stride * height)
                throw new InvalidOperationException(
                    $"The recorded frame is {pixels.Length} bytes, expected {stride * height}.");

            // Row 0 of the readback is the top of the image unless the
            // graphics API is bottom-up (OpenGL).
            var submitResult = video_encoder_submit_rgba(
                encoder,
                (IntPtr)NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(pixels),
                stride,
                SystemInfo.graphicsUVStartsAtTop ? 0 : 1);
            if (submitResult < 0)
                throw new InvalidOperationException(
                    $"RenderingOut failed to encode a video frame ({submitResult}).");
        }
#endif

        private static unsafe void MuxRecordingAudio(IntPtr encoder)
        {
            var pcmData = _audioManager.GetRecordingBuffer(out var sampleCount);
            if (sampleCount == 0)
                return;

            var pcmDataPointer = (IntPtr)NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(pcmData);
            var muxResult = video_encoder_mux_audio(
                encoder,
                pcmDataPointer,
                checked(sampleCount * sizeof(float)),
                AudioManager.SAMPLERATE,
                AudioManager.CHANNELS);
            if (muxResult < 0)
                throw new InvalidOperationException(
                    $"RenderingOut failed to mux the recorded audio ({muxResult}).");
        }

        private static void FreeEncoder(ref IntPtr encoder)
        {
            if (encoder == IntPtr.Zero)
                return;

            var encoderToFree = encoder;
            encoder = IntPtr.Zero;
            video_encoder_free(encoderToFree);
        }



#if UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr ShellExecuteW(
            IntPtr window,
            string operation,
            string file,
            string parameters,
            string directory,
            int showCommand);
#elif UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
    [DllImport("libSystem.dylib", EntryPoint = "system")]
    private static extern int RunSystemCommand(string command);
#elif UNITY_EDITOR_LINUX || UNITY_STANDALONE_LINUX
    [DllImport("libc", EntryPoint = "system")]
    private static extern int RunSystemCommand(string command);
#endif
        private static void OpenFileLocation(string filePath)
        {
            var fullPath = Path.GetFullPath(filePath);

#if UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN
            var result = ShellExecuteW(
                IntPtr.Zero,
                "open",
                "explorer.exe",
                $"/select,\"{fullPath}\"",
                string.Empty,
                1);
            if (result.ToInt64() > 32) return;
#elif UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
        if (RunSystemCommand($"open -R {QuoteShellArgument(fullPath)}") == 0) return;
#elif UNITY_EDITOR_LINUX || UNITY_STANDALONE_LINUX
        var linuxDirectory = Path.GetDirectoryName(fullPath);
        if (linuxDirectory is not null &&
            RunSystemCommand($"xdg-open {QuoteShellArgument(linuxDirectory)} >/dev/null 2>&1 &") == 0)
            return;
#endif

            var directoryPath = Path.GetDirectoryName(fullPath);
            if (directoryPath is not null)
                Application.OpenURL(new Uri(directoryPath + Path.DirectorySeparatorChar).AbsoluteUri);
        }

#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX || UNITY_EDITOR_LINUX || UNITY_STANDALONE_LINUX
    private static string QuoteShellArgument(string value) =>
        $"'{value.Replace("'", "'\"'\"'")}'";
#endif
    }
}