using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace ShinkouUI.Studio.Wpf;

public partial class ColorPickerControl : UserControl
{
    public static readonly DependencyProperty ColorProperty = DependencyProperty.Register(
        nameof(Color), typeof(Color), typeof(ColorPickerControl),
        new FrameworkPropertyMetadata(Colors.White, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, OnColorChanged));

    public static readonly DependencyProperty LabelProperty = DependencyProperty.Register(
        nameof(Label), typeof(string), typeof(ColorPickerControl), new PropertyMetadata("颜色", OnLabelChanged));

    public static readonly DependencyProperty IsDarkProperty = DependencyProperty.Register(
        nameof(IsDark), typeof(bool), typeof(ColorPickerControl), new PropertyMetadata(false, OnThemeChanged));

    private bool _internalUpdate;
    private bool _dragging;
    private double _hue;
    private double _saturation = 1;
    private double _value = 1;
    private Window? _ownerWindow;

    public ColorPickerControl()
    {
        InitializeComponent();
        Loaded += PickerLoaded;
        Unloaded += PickerUnloaded;
    }

    public Color Color
    {
        get => (Color)GetValue(ColorProperty);
        set => SetValue(ColorProperty, value);
    }

    public string Label
    {
        get => (string)GetValue(LabelProperty);
        set => SetValue(LabelProperty, value);
    }

    public bool IsDark
    {
        get => (bool)GetValue(IsDarkProperty);
        set => SetValue(IsDarkProperty, value);
    }

    public event EventHandler? ColorChanged;

    private static void OnColorChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        var picker = (ColorPickerControl)sender;
        picker.UpdateFromColor((Color)args.NewValue);
        picker.ColorChanged?.Invoke(picker, EventArgs.Empty);
    }

    private static void OnLabelChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ColorPickerControl picker && picker.LabelText is not null)
            picker.LabelText.Text = args.NewValue as string ?? "颜色";
    }

    private static void OnThemeChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ColorPickerControl picker && picker.PopupFrame is not null)
            picker.ApplyTheme();
    }

    private void UpdateFromColor(Color color)
    {
        ColorToHsv(color, out _hue, out _saturation, out _value);
        _internalUpdate = true;
        try
        {
            HueSlider.Value = _hue;
            HexBox.Text = color.ToString();
            AlphaBox.Text = color.A.ToString(CultureInfo.InvariantCulture);
            Swatch.Background = new SolidColorBrush(color);
            ValueText.Text = color.ToString();
            HueBase.Background = new SolidColorBrush(HsvToColor(_hue, 1, 1));
            UpdateMarker();
        }
        finally { _internalUpdate = false; }
    }

    private void SwatchButton_OnMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (!PickerPopup.IsOpen) UpdateFromColor(Color);
        PickerPopup.IsOpen = !PickerPopup.IsOpen;
        e.Handled = true;
    }

    private void PickerLoaded(object sender, RoutedEventArgs e)
    {
        var window = Window.GetWindow(this);
        if (ReferenceEquals(_ownerWindow, window)) return;
        DetachOwner();
        _ownerWindow = window;
        if (_ownerWindow is not null)
        {
            _ownerWindow.StateChanged += OwnerWindow_OnStateChanged;
            _ownerWindow.Closing += OwnerWindow_OnClosing;
        }
        ApplyTheme();
    }

    private void PickerUnloaded(object sender, RoutedEventArgs e)
    {
        ClosePicker();
        DetachOwner();
    }

    private void DetachOwner()
    {
        if (_ownerWindow is null) return;
        _ownerWindow.StateChanged -= OwnerWindow_OnStateChanged;
        _ownerWindow.Closing -= OwnerWindow_OnClosing;
        _ownerWindow = null;
    }

    private void OwnerWindow_OnStateChanged(object? sender, EventArgs e)
    {
        if (_ownerWindow?.WindowState == WindowState.Minimized) ClosePicker();
    }

    private void OwnerWindow_OnClosing(object? sender, System.ComponentModel.CancelEventArgs e) => ClosePicker();

    private void PickerPopup_OnClosed(object? sender, EventArgs e)
    {
        _dragging = false;
        if (SvCanvas.IsMouseCaptured) SvCanvas.ReleaseMouseCapture();
    }

    public void ClosePicker()
    {
        _dragging = false;
        if (SvCanvas.IsMouseCaptured) SvCanvas.ReleaseMouseCapture();
        PickerPopup.IsOpen = false;
    }

    private void ClosePicker_OnClick(object sender, RoutedEventArgs e)
    {
        ClosePicker();
        e.Handled = true;
    }

    private void ApplyTheme()
    {
        var panel = IsDark ? "#2C2C2E" : "#FFFFFF";
        var input = IsDark ? "#3A3A3C" : "#F7F8FA";
        var line = IsDark ? "#4A4A4C" : "#D9DDE4";
        var text = IsDark ? "#F5F5F7" : "#20242B";
        var muted = IsDark ? "#A1A1A6" : "#8A909A";
        PopupFrame.Background = BrushFrom(panel);
        PopupFrame.BorderBrush = BrushFrom(line);
        SwatchButton.Background = BrushFrom(input);
        SwatchButton.BorderBrush = BrushFrom(line);
        LabelText.Foreground = BrushFrom(text);
        ValueText.Foreground = BrushFrom(IsDark ? "#D1D1D6" : "#424853");
        ClosePickerButton.Foreground = BrushFrom(muted);
        HexBox.Background = BrushFrom(input); HexBox.BorderBrush = BrushFrom(line); HexBox.Foreground = BrushFrom(text);
        AlphaBox.Background = BrushFrom(input); AlphaBox.BorderBrush = BrushFrom(line); AlphaBox.Foreground = BrushFrom(text);
    }

    private void HueSlider_OnValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_internalUpdate) return;
        _hue = HueSlider.Value;
        HueBase.Background = new SolidColorBrush(HsvToColor(_hue, 1, 1));
        CommitColor(HsvToColor(_hue, _saturation, _value));
    }

    private void SvCanvas_OnMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        _dragging = true;
        SvCanvas.CaptureMouse();
        UpdateSv(e.GetPosition(SvCanvas));
    }

    private void SvCanvas_OnMouseMove(object sender, MouseEventArgs e)
    {
        if (_dragging) UpdateSv(e.GetPosition(SvCanvas));
    }

    private void SvCanvas_OnMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        _dragging = false;
        SvCanvas.ReleaseMouseCapture();
    }

    private void SvCanvas_OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateMarker();

    private void UpdateSv(Point point)
    {
        var width = Math.Max(1, SvCanvas.ActualWidth);
        var height = Math.Max(1, SvCanvas.ActualHeight);
        _saturation = Math.Clamp(point.X / width, 0, 1);
        _value = Math.Clamp(1 - point.Y / height, 0, 1);
        UpdateMarker();
        CommitColor(HsvToColor(_hue, _saturation, _value));
    }

    private void UpdateMarker()
    {
        if (SvCanvas is null) return;
        Canvas.SetLeft(SvMarker, Math.Clamp(_saturation * Math.Max(0, SvCanvas.ActualWidth - SvMarker.Width), 0, Math.Max(0, SvCanvas.ActualWidth - SvMarker.Width)));
        Canvas.SetTop(SvMarker, Math.Clamp((1 - _value) * Math.Max(0, SvCanvas.ActualHeight - SvMarker.Height), 0, Math.Max(0, SvCanvas.ActualHeight - SvMarker.Height)));
    }

    private void HexBox_OnTextChanged(object sender, TextChangedEventArgs e)
    {
        if (_internalUpdate || !TryParseColor(HexBox.Text, out var color)) return;
        CommitColor(color);
    }

    private void AlphaBox_OnTextChanged(object sender, TextChangedEventArgs e)
    {
        if (_internalUpdate || !byte.TryParse(AlphaBox.Text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var alpha)) return;
        var color = Color;
        CommitColor(Color.FromArgb(alpha, color.R, color.G, color.B));
    }

    private void Preset_OnClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button button && TryParseColor(button.Tag?.ToString(), out var color)) CommitColor(color);
    }

    private void CommitColor(Color color)
    {
        if (Color == color) return;
        Color = color;
    }

    private static bool TryParseColor(string? text, out Color color)
    {
        color = Colors.White;
        if (string.IsNullOrWhiteSpace(text)) return false;
        try
        {
            var value = text.Trim();
            if (value.Length == 4 && value[0] == '#')
                value = $"#{value[1]}{value[1]}{value[2]}{value[2]}{value[3]}{value[3]}";
            if (value.Length == 7 && value[0] == '#') value = "#FF" + value[1..];
            color = (Color)ColorConverter.ConvertFromString(value)!;
            return true;
        }
        catch { return false; }
    }

    private static Color HsvToColor(double hue, double saturation, double value)
    {
        var chroma = value * saturation;
        var x = chroma * (1 - Math.Abs((hue / 60 % 2) - 1));
        var match = value - chroma;
        (double r, double g, double b) = hue switch
        {
            < 60 => (chroma, x, 0d), < 120 => (x, chroma, 0d), < 180 => (0d, chroma, x),
            < 240 => (0d, x, chroma), < 300 => (x, 0d, chroma), _ => (chroma, 0d, x)
        };
        return Color.FromArgb(255, ToByte(r + match), ToByte(g + match), ToByte(b + match));
    }

    private static void ColorToHsv(Color color, out double hue, out double saturation, out double value)
    {
        var r = color.R / 255d; var g = color.G / 255d; var b = color.B / 255d;
        var max = Math.Max(r, Math.Max(g, b)); var min = Math.Min(r, Math.Min(g, b)); var delta = max - min;
        hue = delta == 0 ? 0 : max == r ? 60 * (((g - b) / delta + 6) % 6) : max == g ? 60 * ((b - r) / delta + 2) : 60 * ((r - g) / delta + 4);
        saturation = max == 0 ? 0 : delta / max;
        value = max;
    }

    private static byte ToByte(double value) => (byte)Math.Clamp(Math.Round(value * 255), 0, 255);

    private static Brush BrushFrom(string value) => new SolidColorBrush((Color)ColorConverter.ConvertFromString(value)!);
}
