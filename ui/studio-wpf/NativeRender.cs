using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ShinkouUI.Studio.Wpf;

internal sealed class NativeRender : IDisposable
{
    private const string Library = "shinkou_render_native.dll";
    private IntPtr _context;
    private readonly object _gate = new();
    private byte[] _rgba = Array.Empty<byte>();
    private byte[] _bgra = Array.Empty<byte>();
    private uint _bufferWidth;
    private uint _bufferHeight;
    public string? Error { get; private set; }

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr shinkou_render_create(uint width, uint height);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern void shinkou_render_destroy(IntPtr context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern int shinkou_render_begin(IntPtr context, uint width, uint height, uint clearRgba8);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern void shinkou_render_rect(IntPtr context, float x, float y, float width, float height, uint rgba8, float radius);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern void shinkou_render_border(IntPtr context, float x, float y, float width, float height, uint rgba8, float thickness, float radius);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern void shinkou_render_line(IntPtr context, float x1, float y1, float x2, float y2, uint rgba8, float thickness);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern void shinkou_render_gradient(IntPtr context, float x, float y, float width, float height, uint startRgba8, uint endRgba8);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern int shinkou_render_end(IntPtr context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr shinkou_render_pixels(IntPtr context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong shinkou_render_frames(IntPtr context);

    public NativeRender(uint width, uint height)
    {
        try
        {
            _context = shinkou_render_create(width, height);
            if (_context == IntPtr.Zero) Error = "ShinkouRender 初始化失败。";
        }
        catch (DllNotFoundException exception) { Error = $"找不到 ShinkouRender DLL：{exception.Message}"; }
        catch (BadImageFormatException exception) { Error = $"ShinkouRender DLL 位数不匹配：{exception.Message}"; }
    }

    public byte[]? Render(uint width, uint height, uint clear, uint surface, uint elevated, uint border, uint accent, uint text, int radius)
    {
        lock (_gate)
        {
            if (_context == IntPtr.Zero) return null;
            return RenderLocked(width, height, clear, surface, elevated, border, accent, text, radius);
        }
    }

    private byte[]? RenderLocked(uint width, uint height, uint clear, uint surface, uint elevated, uint border, uint accent, uint text, int radius)
    {
        try
        {
            if (shinkou_render_begin(_context, width, height, clear) == 0) return null;
            shinkou_render_gradient(_context, 0, 0, width, height, clear, surface);
            shinkou_render_rect(_context, 22, 20, width - 44, 52, elevated, 8);
            shinkou_render_border(_context, 22, 20, width - 44, 52, border, 1, 8);
            shinkou_render_rect(_context, 22, 92, width - 44, height - 114, surface, 8);
            shinkou_render_border(_context, 22, 92, width - 44, height - 114, border, 1, 8);
            shinkou_render_rect(_context, 50, 170, 230, 192, clear, 6);
            shinkou_render_border(_context, 50, 170, 230, 192, border, 1, 6);
            shinkou_render_rect(_context, 76, 232, 178, 38, surface, radius);
            shinkou_render_border(_context, 76, 232, 178, 38, border, 1, radius);
            shinkou_render_rect(_context, 76, 286, 178, 38, accent, radius);
            shinkou_render_rect(_context, 316, 170, width - 366, 192, clear, 6);
            shinkou_render_border(_context, 316, 170, width - 366, 192, border, 1, 6);
            shinkou_render_rect(_context, 342, 232, width - 418, 36, surface, 3);
            shinkou_render_border(_context, 342, 232, width - 418, 36, border, 1, 3);
            shinkou_render_line(_context, 342, 318, width - 72, 318, border, 3);
            shinkou_render_line(_context, 342, 318, 520, 318, accent, 3);
            shinkou_render_rect(_context, 513, 311, 14, 14, accent, 7);
            shinkou_render_rect(_context, 50, 390, width - 100, 72, elevated, 0);
            if (shinkou_render_end(_context) == 0) return null;
            var bytes = checked((int)(width * height * 4));
            if (_bufferWidth != width || _bufferHeight != height || _rgba.Length != bytes)
            {
                _rgba = new byte[bytes];
                _bgra = new byte[bytes];
                _bufferWidth = width;
                _bufferHeight = height;
            }
            Marshal.Copy(shinkou_render_pixels(_context), _rgba, 0, _rgba.Length);
            for (var index = 0; index < _rgba.Length; index += 4)
            {
                _bgra[index] = _rgba[index + 2];
                _bgra[index + 1] = _rgba[index + 1];
                _bgra[index + 2] = _rgba[index];
                _bgra[index + 3] = _rgba[index + 3];
            }
            return _bgra;
        }
        catch (DllNotFoundException exception) { Error = $"找不到 ShinkouRender DLL：{exception.Message}"; return null; }
        catch (EntryPointNotFoundException exception) { Error = $"ShinkouRender ABI 不匹配：{exception.Message}"; return null; }
    }

    public ulong Frames => _context == IntPtr.Zero ? 0 : shinkou_render_frames(_context);
    public void Dispose() { lock (_gate) { if (_context != IntPtr.Zero) { shinkou_render_destroy(_context); _context = IntPtr.Zero; } } GC.SuppressFinalize(this); }
}
