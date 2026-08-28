using System;
using System.Runtime.InteropServices;

namespace ShinkouUI.Studio.Wpf;

internal sealed class NativeEditor : IDisposable
{
    private IntPtr _handle;

    public NativeEditor(float width, float height)
    {
        try
        {
            _handle = Create(width, height);
        }
        catch (DllNotFoundException)
        {
            _handle = IntPtr.Zero;
        }
        catch (EntryPointNotFoundException)
        {
            _handle = IntPtr.Zero;
        }
    }

    public bool IsAvailable => _handle != IntPtr.Zero;

    public ulong Add(string type) => IsAvailable ? AddNative(_handle, type, 0) : 0;
    public ulong Duplicate(ulong id) => IsAvailable ? DuplicateNative(_handle, id) : 0;
    public bool Remove(ulong id) => IsAvailable && RemoveNative(_handle, id) != 0;
    public bool Select(ulong id) => IsAvailable && SelectNative(_handle, id) != 0;
    public bool SetString(ulong id, string property, string value) => IsAvailable && SetStringNative(_handle, id, property, value) != 0;
    public bool SetFloat(ulong id, string property, float value) => IsAvailable && SetFloatNative(_handle, id, property, value) != 0;
    public bool SetBool(ulong id, string property, bool value) => IsAvailable && SetBoolNative(_handle, id, property, value ? 1 : 0) != 0;
    public bool Undo() => IsAvailable && UndoNative(_handle) != 0;
    public bool Redo() => IsAvailable && RedoNative(_handle) != 0;
    public bool Reset() => IsAvailable && LoadNative(_handle, "<ui/>") != 0;

    public string Serialize()
    {
        if (!IsAvailable) return string.Empty;
        var required = SerializeNative(_handle, IntPtr.Zero, 0);
        if (required <= 1 || required > 16 * 1024 * 1024) return string.Empty;
        var buffer = Marshal.AllocHGlobal((int)required);
        try
        {
            SerializeNative(_handle, buffer, required);
            return Marshal.PtrToStringUTF8(buffer) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public void Dispose()
    {
        if (_handle == IntPtr.Zero) return;
        Destroy(_handle);
        _handle = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }

    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_create", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr Create(float width, float height);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_destroy", CallingConvention = CallingConvention.Cdecl)]
    private static extern void Destroy(IntPtr handle);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_add", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong AddNative(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string type, ulong parent);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_duplicate", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong DuplicateNative(IntPtr handle, ulong id);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_remove", CallingConvention = CallingConvention.Cdecl)]
    private static extern int RemoveNative(IntPtr handle, ulong id);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_select", CallingConvention = CallingConvention.Cdecl)]
    private static extern int SelectNative(IntPtr handle, ulong id);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_set_string", CallingConvention = CallingConvention.Cdecl)]
    private static extern int SetStringNative(IntPtr handle, ulong id, [MarshalAs(UnmanagedType.LPUTF8Str)] string property, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_set_float", CallingConvention = CallingConvention.Cdecl)]
    private static extern int SetFloatNative(IntPtr handle, ulong id, [MarshalAs(UnmanagedType.LPUTF8Str)] string property, float value);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_set_bool", CallingConvention = CallingConvention.Cdecl)]
    private static extern int SetBoolNative(IntPtr handle, ulong id, [MarshalAs(UnmanagedType.LPUTF8Str)] string property, int value);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_undo", CallingConvention = CallingConvention.Cdecl)]
    private static extern int UndoNative(IntPtr handle);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_redo", CallingConvention = CallingConvention.Cdecl)]
    private static extern int RedoNative(IntPtr handle);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_serialize", CallingConvention = CallingConvention.Cdecl)]
    private static extern nuint SerializeNative(IntPtr handle, IntPtr buffer, nuint capacity);
    [DllImport("shinkou_ui_native.dll", EntryPoint = "shinkou_ui_editor_load", CallingConvention = CallingConvention.Cdecl)]
    private static extern int LoadNative(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string source);
}
