using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using System.Windows.Threading;
using System.Xml.Linq;
using Microsoft.Win32;

namespace ShinkouUI.Studio.Wpf;

public partial class MainWindow : Window
{
    private readonly ObservableCollection<StyleDefinition> _styles = new();
    private readonly Stack<List<StyleDefinition>> _undo = new();
    private readonly Stack<List<StyleDefinition>> _redo = new();
    private bool _loading;
    private bool _ready;
    private bool _historyCaptured;
    private Border? _previewFrame;
    private FrameworkElement? _previewContent;
    private TextBox MarginTopBox = null!;
    private TextBox MarginRightBox = null!;
    private TextBox MarginBottomBox = null!;
    private TextBox MarginLeftBox = null!;
    private TextBox PaddingTopBox = null!;
    private TextBox PaddingRightBox = null!;
    private TextBox PaddingBottomBox = null!;
    private TextBox PaddingLeftBox = null!;
    private TextBox BorderTopBox = null!;
    private TextBox BorderRightBox = null!;
    private TextBox BorderBottomBox = null!;
    private TextBox BorderLeftBox = null!;
    private TextBox WidthBox = null!;
    private TextBox HeightBox = null!;
    private TextBox MinWidthBox = null!;
    private TextBox MaxWidthBox = null!;
    private TextBox MinHeightBox = null!;
    private TextBox MaxHeightBox = null!;
    private StackPanel SpecializedInspector = null!;
    private StackPanel WindowInspector = null!;
    private StackPanel LayoutInspector = null!;
    private StackPanel FileViewInspector = null!;
    private TextBox WindowTitleBox = null!;
    private TextBox WindowTitleBarHeightBox = null!;
    private TextBox WindowBackdropOpacityBox = null!;
    private ComboBox WindowChromeBox = null!;
    private CheckBox WindowModalCheck = null!;
    private CheckBox WindowResizableCheck = null!;
    private CheckBox WindowDraggableCheck = null!;
    private CheckBox WindowControlsCheck = null!;
    private ComboBox LayoutModeBox = null!;
    private ComboBox LayoutOrientationBox = null!;
    private ComboBox LayoutAlignmentBox = null!;
    private TextBox LayoutGapBox = null!;
    private TextBox LayoutColumnsBox = null!;
    private TextBox LayoutSplitRatioBox = null!;
    private CheckBox LayoutClipCheck = null!;
    private CheckBox LayoutDividersCheck = null!;
    private ComboBox FileViewModeBox = null!;
    private ComboBox FileSortBox = null!;
    private ComboBox FileSortDirectionBox = null!;
    private TextBox FileIconSizeBox = null!;
    private TextBox FileRowHeightBox = null!;
    private CheckBox FileThumbnailsCheck = null!;
    private CheckBox FileExtensionsCheck = null!;
    private CheckBox FileHiddenCheck = null!;
    private CheckBox FileAlternatingCheck = null!;
    private CheckBox FileGroupingCheck = null!;

    public MainWindow()
    {
        InitializeComponent();
        ConfigureStyleTypePicker();
        FontFamilyBox.Width = 145;
        AnimationEasingBox.Width = 145;
        AnimationRepeatBox.Width = 145;
        StyleList.ItemsSource = _styles;
        SeedStyles();
        Loaded += (_, _) =>
        {
            BuildAdvancedInspector();
            _ready = true;
            StyleList.SelectedIndex = 1;
            RenderPreview();
        };
    }

    private void ConfigureStyleTypePicker()
    {
        NewStyleTypeBox.Items.Clear();
        AddStyleTypeGroup("基础控件", ("按钮", "button"), ("文本框", "textbox"), ("复选框", "checkbox"), ("滑块", "slider"), ("进度条", "progress"));
        AddStyleTypeGroup("容器与窗口", ("面板", "panel"), ("子窗口", "window"), ("对话框", "dialog"), ("布局容器", "layout"));
        AddStyleTypeGroup("编辑器专用", ("文件浏览器", "file-view"));
        NewStyleTypeBox.SelectedIndex = 1;
        NewStyleTypeBox.Width = 154;
    }

    private void AddStyleTypeGroup(string group, params (string Label, string Type)[] entries)
    {
        NewStyleTypeBox.Items.Add(new ComboBoxItem
        {
            Content = group,
            IsEnabled = false,
            FontSize = 10,
            FontWeight = FontWeights.SemiBold,
            Foreground = (Brush)FindResource("MutedBrush"),
            Padding = new Thickness(8, 6, 8, 4)
        });
        foreach (var entry in entries)
        {
            NewStyleTypeBox.Items.Add(new ComboBoxItem
            {
                Content = entry.Label,
                Tag = entry.Type,
                Padding = new Thickness(12, 5, 8, 5)
            });
        }
    }

    private sealed class AnimationDefinition
    {
        public bool Enabled { get; set; }
        public double DurationMs { get; set; } = 180;
        public double DelayMs { get; set; }
        public string Easing { get; set; } = "ease-out";
        public string Repeat { get; set; } = "once";
        public double P1X { get; set; } = 0.25;
        public double P1Y { get; set; } = 0.1;
        public double P2X { get; set; } = 0.25;
        public double P2Y { get; set; } = 1;
        public bool TriggerHover { get; set; } = true;
        public bool TriggerPressed { get; set; } = true;
        public bool TriggerFocus { get; set; }
        public bool TriggerDisabled { get; set; }

        public AnimationDefinition Clone() => new()
        {
            Enabled = Enabled, DurationMs = DurationMs, DelayMs = DelayMs, Easing = Easing, Repeat = Repeat,
            P1X = P1X, P1Y = P1Y, P2X = P2X, P2Y = P2Y, TriggerHover = TriggerHover,
            TriggerPressed = TriggerPressed, TriggerFocus = TriggerFocus, TriggerDisabled = TriggerDisabled
        };
    }

    private sealed class EdgeValues
    {
        public double Top { get; set; }
        public double Right { get; set; }
        public double Bottom { get; set; }
        public double Left { get; set; }

        public EdgeValues(double uniform = 0) => Top = Right = Bottom = Left = uniform;

        public Thickness ToThickness() => new(Left, Top, Right, Bottom);

        public bool IsUniform => Top == Right && Right == Bottom && Bottom == Left;

        public EdgeValues Clone() => new()
        {
            Top = Top, Right = Right, Bottom = Bottom, Left = Left
        };
    }

    private sealed class WindowDefinition
    {
        public string Title { get; set; } = "Shinkou 窗口";
        public string Chrome { get; set; } = "macos";
        public double TitleBarHeight { get; set; } = 34;
        public double BackdropOpacity { get; set; } = 0.35;
        public bool Modal { get; set; }
        public bool Resizable { get; set; } = true;
        public bool Draggable { get; set; } = true;
        public bool ShowControls { get; set; } = true;

        public WindowDefinition Clone() => new()
        {
            Title = Title, Chrome = Chrome, TitleBarHeight = TitleBarHeight, BackdropOpacity = BackdropOpacity,
            Modal = Modal, Resizable = Resizable, Draggable = Draggable, ShowControls = ShowControls
        };
    }

    private sealed class LayoutDefinition
    {
        public string Mode { get; set; } = "stack";
        public string Orientation { get; set; } = "vertical";
        public string Alignment { get; set; } = "stretch";
        public double Gap { get; set; } = 12;
        public double Columns { get; set; } = 2;
        public double SplitRatio { get; set; } = 0.35;
        public bool ClipContent { get; set; } = true;
        public bool ShowDividers { get; set; }

        public LayoutDefinition Clone() => new()
        {
            Mode = Mode, Orientation = Orientation, Alignment = Alignment, Gap = Gap,
            Columns = Columns, SplitRatio = SplitRatio, ClipContent = ClipContent, ShowDividers = ShowDividers
        };
    }

    private sealed class FileViewDefinition
    {
        public string ViewMode { get; set; } = "details";
        public string SortBy { get; set; } = "name";
        public string SortDirection { get; set; } = "asc";
        public double IconSize { get; set; } = 18;
        public double RowHeight { get; set; } = 32;
        public bool ShowThumbnails { get; set; } = true;
        public bool ShowExtensions { get; set; } = true;
        public bool ShowHidden { get; set; }
        public bool AlternatingRows { get; set; } = true;
        public bool GroupByType { get; set; }

        public FileViewDefinition Clone() => new()
        {
            ViewMode = ViewMode, SortBy = SortBy, SortDirection = SortDirection,
            IconSize = IconSize, RowHeight = RowHeight, ShowThumbnails = ShowThumbnails,
            ShowExtensions = ShowExtensions, ShowHidden = ShowHidden, AlternatingRows = AlternatingRows,
            GroupByType = GroupByType
        };
    }

    private sealed class StyleDefinition
    {
        public string Type { get; set; } = "button";
        public string Variant { get; set; } = "default";
        public string Background { get; set; } = "#F1F3F6";
        public string Foreground { get; set; } = "#20242B";
        public string Border { get; set; } = "#DDE1E7";
        public string HoverBackground { get; set; } = "#E8EDF3";
        public string PressedBackground { get; set; } = "#D9E0E8";
        public string FontFamily { get; set; } = "Microsoft YaHei";
        public string FontWeightName { get; set; } = "Regular";
        public double Radius { get; set; } = 6;
        public EdgeValues Margin { get; set; } = new();
        public EdgeValues Padding { get; set; } = new(12);
        public double FontSize { get; set; } = 14;
        public EdgeValues BorderThickness { get; set; } = new(1);
        public double Width { get; set; }
        public double Height { get; set; }
        public double MinWidth { get; set; }
        public double MaxWidth { get; set; }
        public double MinHeight { get; set; }
        public double MaxHeight { get; set; }
        public bool Shadow { get; set; }
        public bool ReduceMotion { get; set; }
        public WindowDefinition Window { get; set; } = new();
        public LayoutDefinition Layout { get; set; } = new();
        public FileViewDefinition FileView { get; set; } = new();
        public AnimationDefinition Animation { get; set; } = new();
        public string DisplayName => $"{Type}  /  {Variant}";

        public StyleDefinition Clone() => new()
        {
            Type = Type, Variant = Variant, Background = Background, Foreground = Foreground,
            Border = Border, HoverBackground = HoverBackground, PressedBackground = PressedBackground,
            FontFamily = FontFamily, FontWeightName = FontWeightName, Radius = Radius, Margin = Margin.Clone(), Padding = Padding.Clone(),
            FontSize = FontSize, BorderThickness = BorderThickness.Clone(), Width = Width, Height = Height,
            MinWidth = MinWidth, MaxWidth = MaxWidth, MinHeight = MinHeight, MaxHeight = MaxHeight,
            Shadow = Shadow, ReduceMotion = ReduceMotion, Window = Window.Clone(), Layout = Layout.Clone(), FileView = FileView.Clone(),
            Animation = Animation.Clone()
        };
    }

    private void SeedStyles()
    {
        _styles.Add(new StyleDefinition { Type = "button", Variant = "default", Animation = NewAnimation(true) });
        _styles.Add(new StyleDefinition { Type = "button", Variant = "primary", Background = "#0A84FF", Foreground = "#FFFFFF", Border = "#0A84FF", HoverBackground = "#3195FF", PressedBackground = "#006EDB", FontWeightName = "SemiBold", Animation = NewAnimation(true) });
        _styles.Add(new StyleDefinition { Type = "button", Variant = "destructive", Background = "#D64B4B", Foreground = "#FFFFFF", Border = "#D64B4B", HoverBackground = "#E16363", PressedBackground = "#B83B3B", Animation = NewAnimation(true) });
        _styles.Add(new StyleDefinition { Type = "textbox", Variant = "default", Background = "#FFFFFF", HoverBackground = "#FFFFFF", PressedBackground = "#FFFFFF" });
        _styles.Add(new StyleDefinition { Type = "checkbox", Variant = "default", Background = "#FFFFFF", Border = "#B8C0CC", HoverBackground = "#F0F7FF", PressedBackground = "#E1EFFF" });
        _styles.Add(new StyleDefinition { Type = "slider", Variant = "default", Background = "#0A84FF", Border = "#0A84FF", HoverBackground = "#3195FF", PressedBackground = "#006EDB", Animation = NewAnimation(true) });
        _styles.Add(new StyleDefinition { Type = "progress", Variant = "default", Background = "#0A84FF", Border = "#DDE1E7" });
        _styles.Add(new StyleDefinition { Type = "panel", Variant = "default", Background = "#FFFFFF", HoverBackground = "#FFFFFF", PressedBackground = "#FFFFFF", Radius = 10, Padding = new EdgeValues(16) });
        _styles.Add(new StyleDefinition { Type = "window", Variant = "default", Background = "#FFFFFF", HoverBackground = "#FFFFFF", PressedBackground = "#FFFFFF", Radius = 12, Padding = new EdgeValues(0), Window = new WindowDefinition { Title = "项目窗口", Chrome = "macos", ShowControls = true }, Animation = NewAnimation(true) });
        _styles.Add(new StyleDefinition { Type = "dialog", Variant = "default", Background = "#FFFFFF", HoverBackground = "#FFFFFF", PressedBackground = "#FFFFFF", Radius = 12, Padding = new EdgeValues(18), Window = new WindowDefinition { Title = "确认操作", Chrome = "minimal", Modal = true, Resizable = false }, Animation = NewAnimation(true) });
        _styles.Add(new StyleDefinition { Type = "layout", Variant = "default", Background = "#F5F6F8", HoverBackground = "#F5F6F8", PressedBackground = "#F5F6F8", Radius = 10, Padding = new EdgeValues(12), Layout = new LayoutDefinition { Mode = "split", Orientation = "horizontal", Gap = 10, SplitRatio = 0.34, ShowDividers = true } });
        _styles.Add(new StyleDefinition { Type = "file-view", Variant = "details", Background = "#FFFFFF", HoverBackground = "#F2F7FD", PressedBackground = "#E4EFFC", Radius = 8, Padding = new EdgeValues(8), FileView = new FileViewDefinition { ViewMode = "details", RowHeight = 30, IconSize = 18, ShowThumbnails = false, AlternatingRows = true } });
        _styles.Add(new StyleDefinition { Type = "file-view", Variant = "list", Background = "#FFFFFF", HoverBackground = "#F2F7FD", PressedBackground = "#E4EFFC", Radius = 8, Padding = new EdgeValues(8), FileView = new FileViewDefinition { ViewMode = "list", RowHeight = 36, IconSize = 22, ShowThumbnails = false, AlternatingRows = false } });
        _styles.Add(new StyleDefinition { Type = "file-view", Variant = "tiles", Background = "#FFFFFF", HoverBackground = "#F2F7FD", PressedBackground = "#E4EFFC", Radius = 8, Padding = new EdgeValues(10), FileView = new FileViewDefinition { ViewMode = "tiles", RowHeight = 88, IconSize = 32, ShowThumbnails = true, AlternatingRows = false } });
        _styles.Add(new StyleDefinition { Type = "file-view", Variant = "content", Background = "#FFFFFF", HoverBackground = "#F2F7FD", PressedBackground = "#E4EFFC", Radius = 8, Padding = new EdgeValues(10), FileView = new FileViewDefinition { ViewMode = "content", RowHeight = 64, IconSize = 40, ShowThumbnails = true, AlternatingRows = false } });
    }

    private static AnimationDefinition NewAnimation(bool enabled) => new() { Enabled = enabled, Easing = "ease-out" };
    private StyleDefinition? Current => StyleList.SelectedItem as StyleDefinition;
    private List<StyleDefinition> Snapshot() => _styles.Select(item => item.Clone()).ToList();

    private void BuildAdvancedInspector()
    {
        if (PaddingSlider.Parent is StackPanel legacyPaddingPanel)
            legacyPaddingPanel.Visibility = Visibility.Collapsed;

        var inspectorStack = FindVisualChildren<StackPanel>(InspectorFrame)
            .FirstOrDefault(panel => panel.Children.Contains(VariantNameBox));
        if (inspectorStack is null) return;

        var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 14) };
        panel.Children.Add(new TextBlock { Text = "盒模型与尺寸", Style = (Style)FindResource("SectionTitle") });
        panel.Children.Add(CreateEdgeEditor("外边距 Margin", 0, out MarginTopBox, out MarginRightBox, out MarginBottomBox, out MarginLeftBox));
        panel.Children.Add(CreateEdgeEditor("内边距 Padding", 0, out PaddingTopBox, out PaddingRightBox, out PaddingBottomBox, out PaddingLeftBox));
        panel.Children.Add(CreateEdgeEditor("边框宽度 Border", 1, out BorderTopBox, out BorderRightBox, out BorderBottomBox, out BorderLeftBox));
        panel.Children.Add(CreateDimensionEditor());
        var specializedPanel = CreateSpecializedInspector();

        var firstSeparator = inspectorStack.Children.OfType<Separator>().FirstOrDefault();
        var insertIndex = firstSeparator is null ? inspectorStack.Children.Count : inspectorStack.Children.IndexOf(firstSeparator);
        inspectorStack.Children.Insert(insertIndex, panel);
        inspectorStack.Children.Insert(insertIndex + 1, specializedPanel);
        SpecializedInspector = specializedPanel;
    }

    private StackPanel CreateSpecializedInspector()
    {
        var panel = new StackPanel { Visibility = Visibility.Collapsed, Margin = new Thickness(0, 12, 0, 0) };
        panel.Children.Add(new Separator { Background = (Brush)FindResource("LineBrush"), Margin = new Thickness(0, 0, 0, 16) });
        panel.Children.Add(new TextBlock { Text = "组件专属属性", Style = (Style)FindResource("SectionTitle") });

        WindowInspector = new StackPanel { Visibility = Visibility.Collapsed };
        WindowInspector.Children.Add(new TextBlock { Text = "子窗口 / 对话框", Style = (Style)FindResource("InspectorLabel") });
        WindowTitleBox = CreateTextBox("Shinkou 窗口");
        WindowInspector.Children.Add(CreateLabeledControl("标题", WindowTitleBox));
        WindowTitleBarHeightBox = CreateNumericBox(34);
        WindowInspector.Children.Add(CreateLabeledControl("标题栏高度", WindowTitleBarHeightBox));
        WindowBackdropOpacityBox = CreateNumericBox(0.35);
        WindowInspector.Children.Add(CreateLabeledControl("遮罩透明度（0 - 1）", WindowBackdropOpacityBox));
        WindowChromeBox = CreateAdvancedCombo(("macOS", "macos"), ("Windows", "windows"), ("极简", "minimal"));
        WindowInspector.Children.Add(CreateLabeledControl("窗口外壳", WindowChromeBox));
        WindowInspector.Children.Add(CreateCheckRow(
            WindowModalCheck = CreateAdvancedCheck("模态遮罩"),
            WindowResizableCheck = CreateAdvancedCheck("允许调整大小")));
        WindowInspector.Children.Add(CreateCheckRow(
            WindowDraggableCheck = CreateAdvancedCheck("允许拖动"),
            WindowControlsCheck = CreateAdvancedCheck("显示窗口控制按钮")));
        panel.Children.Add(WindowInspector);

        LayoutInspector = new StackPanel { Visibility = Visibility.Collapsed };
        LayoutInspector.Children.Add(new TextBlock { Text = "布局容器", Style = (Style)FindResource("InspectorLabel") });
        LayoutModeBox = CreateAdvancedCombo(("堆叠 Stack", "stack"), ("网格 Grid", "grid"), ("分栏 Split", "split"), ("覆盖 Overlay", "overlay"));
        LayoutInspector.Children.Add(CreateLabeledControl("布局模式", LayoutModeBox));
        LayoutOrientationBox = CreateAdvancedCombo(("水平", "horizontal"), ("垂直", "vertical"));
        LayoutInspector.Children.Add(CreateLabeledControl("方向", LayoutOrientationBox));
        LayoutAlignmentBox = CreateAdvancedCombo(("拉伸 Stretch", "stretch"), ("开始 Start", "start"), ("居中 Center", "center"), ("末尾 End", "end"), ("两端分布 SpaceBetween", "space-between"));
        LayoutInspector.Children.Add(CreateLabeledControl("对齐策略", LayoutAlignmentBox));
        LayoutGapBox = CreateNumericBox(12);
        LayoutInspector.Children.Add(CreateLabeledControl("间距 Gap", LayoutGapBox));
        LayoutColumnsBox = CreateNumericBox(2);
        LayoutInspector.Children.Add(CreateLabeledControl("网格列数", LayoutColumnsBox));
        LayoutSplitRatioBox = CreateNumericBox(0.35);
        LayoutInspector.Children.Add(CreateLabeledControl("分栏比例（0 - 1）", LayoutSplitRatioBox));
        LayoutInspector.Children.Add(CreateCheckRow(
            LayoutClipCheck = CreateAdvancedCheck("裁切溢出内容"),
            LayoutDividersCheck = CreateAdvancedCheck("显示分隔线")));
        panel.Children.Add(LayoutInspector);

        FileViewInspector = new StackPanel { Visibility = Visibility.Collapsed };
        FileViewInspector.Children.Add(new TextBlock { Text = "文件浏览器视图", Style = (Style)FindResource("InspectorLabel") });
        FileViewModeBox = CreateAdvancedCombo(("详细信息", "details"), ("列表", "list"), ("平铺", "tiles"), ("内容", "content"));
        FileViewInspector.Children.Add(CreateLabeledControl("视图模式", FileViewModeBox));
        FileSortBox = CreateAdvancedCombo(("名称", "name"), ("修改日期", "modified"), ("类型", "type"), ("大小", "size"));
        FileViewInspector.Children.Add(CreateLabeledControl("排序字段", FileSortBox));
        FileSortDirectionBox = CreateAdvancedCombo(("升序", "asc"), ("降序", "desc"));
        FileViewInspector.Children.Add(CreateLabeledControl("排序方向", FileSortDirectionBox));
        FileIconSizeBox = CreateNumericBox(18);
        FileViewInspector.Children.Add(CreateLabeledControl("图标/缩略图尺寸", FileIconSizeBox));
        FileRowHeightBox = CreateNumericBox(32);
        FileViewInspector.Children.Add(CreateLabeledControl("行高/卡片高度", FileRowHeightBox));
        FileViewInspector.Children.Add(CreateCheckRow(
            FileThumbnailsCheck = CreateAdvancedCheck("显示缩略图"),
            FileExtensionsCheck = CreateAdvancedCheck("显示扩展名")));
        FileViewInspector.Children.Add(CreateCheckRow(
            FileHiddenCheck = CreateAdvancedCheck("显示隐藏文件"),
            FileAlternatingCheck = CreateAdvancedCheck("交替行背景")));
        FileViewInspector.Children.Add(CreateCheckRow(
            FileGroupingCheck = CreateAdvancedCheck("按类型分组")));
        panel.Children.Add(FileViewInspector);
        return panel;
    }

    private StackPanel CreateLabeledControl(string label, Control control)
    {
        var group = new StackPanel { Margin = new Thickness(0, 0, 0, 9) };
        group.Children.Add(new TextBlock { Text = label, Style = (Style)FindResource("InspectorLabel") });
        group.Children.Add(control);
        return group;
    }

    private StackPanel CreateCheckRow(params CheckBox[] checks)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 10) };
        for (var i = 0; i < checks.Length; i++)
        {
            checks[i].Margin = new Thickness(0, 0, i == checks.Length - 1 ? 0 : 14, 0);
            row.Children.Add(checks[i]);
        }
        return row;
    }

    private TextBox CreateTextBox(string value)
    {
        var box = new TextBox { Text = value, HorizontalContentAlignment = HorizontalAlignment.Left };
        box.Style = (Style)FindResource("EditorTextBox");
        box.TextChanged += StyleValue_OnChanged;
        return box;
    }

    private ComboBox CreateAdvancedCombo(params (string Content, string Tag)[] entries)
    {
        var combo = new ComboBox { Style = (Style)FindResource("EditorCombo") };
        foreach (var entry in entries) combo.Items.Add(new ComboBoxItem { Content = entry.Content, Tag = entry.Tag });
        combo.SelectionChanged += StyleValue_OnChanged;
        return combo;
    }

    private CheckBox CreateAdvancedCheck(string content)
    {
        var check = new CheckBox { Content = content, Foreground = (Brush)FindResource("TextBrush"), FontSize = 11 };
        check.Checked += StyleValue_OnChanged;
        check.Unchecked += StyleValue_OnChanged;
        return check;
    }

    private StackPanel CreateEdgeEditor(string title, double defaultValue, out TextBox top, out TextBox right, out TextBox bottom, out TextBox left)
    {
        var group = new StackPanel { Margin = new Thickness(0, 0, 0, 9) };
        group.Children.Add(new TextBlock { Text = title, Style = (Style)FindResource("InspectorLabel") });
        var labels = new[] { "上", "右", "下", "左" };
        var fields = new TextBox[4];
        var grid = new Grid();
        for (var i = 0; i < 4; i++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var cell = new StackPanel { Margin = new Thickness(i == 0 ? 0 : 4, 0, 0, 0) };
            cell.Children.Add(new TextBlock { Text = labels[i], FontSize = 9, Foreground = (Brush)FindResource("MutedBrush"), Margin = new Thickness(0, 0, 0, 3) });
            fields[i] = CreateNumericBox(defaultValue);
            cell.Children.Add(fields[i]);
            Grid.SetColumn(cell, i);
            grid.Children.Add(cell);
        }
        group.Children.Add(grid);
        top = fields[0]; right = fields[1]; bottom = fields[2]; left = fields[3];
        return group;
    }

    private StackPanel CreateDimensionEditor()
    {
        var group = new StackPanel();
        group.Children.Add(new TextBlock { Text = "尺寸约束（0 = auto）", Style = (Style)FindResource("InspectorLabel") });
        var grid = new Grid();
        for (var i = 0; i < 4; i++) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        AddDimensionCell(grid, 0, "宽", out WidthBox);
        AddDimensionCell(grid, 1, "高", out HeightBox);
        AddDimensionCell(grid, 2, "最小宽", out MinWidthBox);
        AddDimensionCell(grid, 3, "最大宽", out MaxWidthBox);
        group.Children.Add(grid);

        var second = new Grid { Margin = new Thickness(0, 7, 0, 0) };
        for (var i = 0; i < 4; i++) second.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        AddDimensionCell(second, 0, "最小高", out MinHeightBox);
        AddDimensionCell(second, 1, "最大高", out MaxHeightBox);
        group.Children.Add(second);
        return group;
    }

    private void AddDimensionCell(Grid grid, int column, string label, out TextBox box)
    {
        var cell = new StackPanel { Margin = new Thickness(column == 0 ? 0 : 4, 0, 0, 0) };
        cell.Children.Add(new TextBlock { Text = label, FontSize = 9, Foreground = (Brush)FindResource("MutedBrush"), Margin = new Thickness(0, 0, 0, 3) });
        box = CreateNumericBox(0);
        cell.Children.Add(box);
        Grid.SetColumn(cell, column);
        grid.Children.Add(cell);
    }

    private TextBox CreateNumericBox(double value)
    {
        var box = new TextBox { Text = OptionalNumber(value), HorizontalContentAlignment = HorizontalAlignment.Left };
        box.Style = (Style)FindResource("EditorTextBox");
        box.TextChanged += StyleValue_OnChanged;
        return box;
    }

    private void CaptureHistory()
    {
        if (_loading || !_ready || Current is null || _historyCaptured) return;
        _undo.Push(Snapshot());
        _redo.Clear();
        _historyCaptured = true;
    }

    private void MarkDirty(string message)
    {
        DocumentTitleText.Text = "ShinkouUI Style Studio · 未保存";
        StatusText.Text = message;
    }

    private void StyleList_OnChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_ready || e.Source != StyleList) return;
        _historyCaptured = false;
        UpdateInspector();
        RenderPreview();
        Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, new Action(() =>
        {
            if (!_ready) return;
            _loading = true;
            try { UpdateSpecializedInspector(Current); }
            finally { _loading = false; }
        }));
    }

    private void UpdateInspector()
    {
        var style = Current;
        _loading = true;
        try
        {
            var caption = style is null ? "未选择样式" : $"{style.Type} / {style.Variant}";
            InspectorCaption.Text = caption;
            PreviewCaption.Text = caption;
            VariantNameBox.Text = style?.Variant ?? string.Empty;
            BackgroundPicker.Color = ParseColor(style?.Background, Colors.White);
            ForegroundPicker.Color = ParseColor(style?.Foreground, Colors.Black);
            BorderPicker.Color = ParseColor(style?.Border, Colors.Transparent);
            HoverPicker.Color = ParseColor(style?.HoverBackground, Colors.Transparent);
            PressedPicker.Color = ParseColor(style?.PressedBackground, Colors.Transparent);
            RadiusSlider.Value = style?.Radius ?? 0;
            FontSizeSlider.Value = style?.FontSize ?? 14;
            RadiusValue.Text = $"{RadiusSlider.Value:0} px";
            FontSizeValue.Text = $"{FontSizeSlider.Value:0} px";
            SetEdgeFields(style?.Margin ?? new EdgeValues(), MarginTopBox, MarginRightBox, MarginBottomBox, MarginLeftBox);
            SetEdgeFields(style?.Padding ?? new EdgeValues(), PaddingTopBox, PaddingRightBox, PaddingBottomBox, PaddingLeftBox);
            SetEdgeFields(style?.BorderThickness ?? new EdgeValues(1), BorderTopBox, BorderRightBox, BorderBottomBox, BorderLeftBox);
            WidthBox.Text = OptionalNumber(style?.Width ?? 0);
            HeightBox.Text = OptionalNumber(style?.Height ?? 0);
            MinWidthBox.Text = OptionalNumber(style?.MinWidth ?? 0);
            MaxWidthBox.Text = OptionalNumber(style?.MaxWidth ?? 0);
            MinHeightBox.Text = OptionalNumber(style?.MinHeight ?? 0);
            MaxHeightBox.Text = OptionalNumber(style?.MaxHeight ?? 0);
            FontFamilyBox.SelectedItem = FontFamilyBox.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Content == style?.FontFamily) ?? FontFamilyBox.Items[0];
            FontWeightBox.SelectedItem = FontWeightBox.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Content == style?.FontWeightName) ?? FontWeightBox.Items[0];
            ShadowCheck.IsChecked = style?.Shadow ?? false;
            ReduceMotionCheck.IsChecked = style?.ReduceMotion ?? false;
            UpdateAnimationInspector(style?.Animation);
            UpdateSpecializedInspector(style);
        }
        finally { _loading = false; }
    }

    private void UpdateSpecializedInspector(StyleDefinition? style)
    {
        if (SpecializedInspector is null) return;
        var type = style?.Type.Trim().ToLowerInvariant();
        var isWindow = type is "window" or "dialog";
        var isLayout = type is "layout" or "split-view";
        var isFileView = type is "file-view";
        SpecializedInspector.Visibility = style is null || (!isWindow && !isLayout && !isFileView) ? Visibility.Collapsed : Visibility.Visible;
        WindowInspector.Visibility = isWindow ? Visibility.Visible : Visibility.Collapsed;
        LayoutInspector.Visibility = isLayout ? Visibility.Visible : Visibility.Collapsed;
        FileViewInspector.Visibility = isFileView ? Visibility.Visible : Visibility.Collapsed;
        if (style is null) return;

        if (isWindow)
        {
            WindowTitleBox.Text = style.Window.Title;
            WindowTitleBarHeightBox.Text = OptionalNumber(style.Window.TitleBarHeight);
            WindowBackdropOpacityBox.Text = style.Window.BackdropOpacity.ToString("0.##", CultureInfo.InvariantCulture);
            SelectTag(WindowChromeBox, style.Window.Chrome, "macos");
            WindowModalCheck.IsChecked = style.Window.Modal;
            WindowResizableCheck.IsChecked = style.Window.Resizable;
            WindowDraggableCheck.IsChecked = style.Window.Draggable;
            WindowControlsCheck.IsChecked = style.Window.ShowControls;
        }
        else if (isLayout)
        {
            SelectTag(LayoutModeBox, style.Layout.Mode, "stack");
            SelectTag(LayoutOrientationBox, style.Layout.Orientation, "vertical");
            SelectTag(LayoutAlignmentBox, style.Layout.Alignment, "stretch");
            LayoutGapBox.Text = OptionalNumber(style.Layout.Gap);
            LayoutColumnsBox.Text = OptionalNumber(style.Layout.Columns);
            LayoutSplitRatioBox.Text = style.Layout.SplitRatio.ToString("0.##", CultureInfo.InvariantCulture);
            LayoutClipCheck.IsChecked = style.Layout.ClipContent;
            LayoutDividersCheck.IsChecked = style.Layout.ShowDividers;
        }
        else if (isFileView)
        {
            SelectTag(FileViewModeBox, style.FileView.ViewMode, "details");
            SelectTag(FileSortBox, style.FileView.SortBy, "name");
            SelectTag(FileSortDirectionBox, style.FileView.SortDirection, "asc");
            FileIconSizeBox.Text = OptionalNumber(style.FileView.IconSize);
            FileRowHeightBox.Text = OptionalNumber(style.FileView.RowHeight);
            FileThumbnailsCheck.IsChecked = style.FileView.ShowThumbnails;
            FileExtensionsCheck.IsChecked = style.FileView.ShowExtensions;
            FileHiddenCheck.IsChecked = style.FileView.ShowHidden;
            FileAlternatingCheck.IsChecked = style.FileView.AlternatingRows;
            FileGroupingCheck.IsChecked = style.FileView.GroupByType;
        }
    }

    private static void SelectTag(ComboBox combo, string value, string fallback)
    {
        combo.SelectedItem = combo.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Tag == value)
            ?? combo.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Tag == fallback)
            ?? combo.Items[0];
    }

    private void UpdateAnimationInspector(AnimationDefinition? animation)
    {
        animation ??= new AnimationDefinition();
        AnimationEnabledCheck.IsChecked = animation.Enabled;
        AnimationDurationSlider.Value = animation.DurationMs;
        AnimationDelaySlider.Value = animation.DelayMs;
        AnimationDurationValue.Text = $"{animation.DurationMs:0} ms";
        AnimationDelayValue.Text = $"{animation.DelayMs:0} ms";
        AnimationEasingBox.SelectedItem = AnimationEasingBox.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Tag == animation.Easing) ?? AnimationEasingBox.Items[1];
        AnimationRepeatBox.SelectedItem = AnimationRepeatBox.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Tag == animation.Repeat) ?? AnimationRepeatBox.Items[0];
        BezierP1XBox.Text = animation.P1X.ToString("0.##", CultureInfo.InvariantCulture);
        BezierP1YBox.Text = animation.P1Y.ToString("0.##", CultureInfo.InvariantCulture);
        BezierP2XBox.Text = animation.P2X.ToString("0.##", CultureInfo.InvariantCulture);
        BezierP2YBox.Text = animation.P2Y.ToString("0.##", CultureInfo.InvariantCulture);
        TriggerHoverCheck.IsChecked = animation.TriggerHover;
        TriggerPressedCheck.IsChecked = animation.TriggerPressed;
        TriggerFocusCheck.IsChecked = animation.TriggerFocus;
        TriggerDisabledCheck.IsChecked = animation.TriggerDisabled;
    }

    private void StyleValue_OnChanged(object sender, RoutedEventArgs e) => ApplyInspector();
    private void StyleValue_OnChanged(object sender, TextChangedEventArgs e) => ApplyInspector();
    private void StyleValue_OnChanged(object sender, SelectionChangedEventArgs e) => ApplyInspector();
    private void StyleValue_OnChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => ApplyInspector();

    private void ApplyInspector()
    {
        var style = Current;
        if (!_ready || _loading || style is null) return;
        CaptureHistory();
        style.Variant = VariantNameBox.Text.Trim().Length == 0 ? "default" : VariantNameBox.Text.Trim();
        style.Background = BackgroundPicker.Color.ToString();
        style.Foreground = ForegroundPicker.Color.ToString();
        style.Border = BorderPicker.Color.ToString();
        style.HoverBackground = HoverPicker.Color.ToString();
        style.PressedBackground = PressedPicker.Color.ToString();
        style.Radius = RadiusSlider.Value;
        style.Margin = ReadEdgeFields(style.Margin, MarginTopBox, MarginRightBox, MarginBottomBox, MarginLeftBox);
        style.Padding = ReadEdgeFields(style.Padding, PaddingTopBox, PaddingRightBox, PaddingBottomBox, PaddingLeftBox);
        style.BorderThickness = ReadEdgeFields(style.BorderThickness, BorderTopBox, BorderRightBox, BorderBottomBox, BorderLeftBox);
        style.Width = OptionalNumber(WidthBox.Text, style.Width);
        style.Height = OptionalNumber(HeightBox.Text, style.Height);
        style.MinWidth = OptionalNumber(MinWidthBox.Text, style.MinWidth);
        style.MaxWidth = OptionalNumber(MaxWidthBox.Text, style.MaxWidth);
        style.MinHeight = OptionalNumber(MinHeightBox.Text, style.MinHeight);
        style.MaxHeight = OptionalNumber(MaxHeightBox.Text, style.MaxHeight);
        style.FontSize = FontSizeSlider.Value;
        style.FontFamily = (FontFamilyBox.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "Microsoft YaHei";
        style.FontWeightName = (FontWeightBox.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "Regular";
        style.Shadow = ShadowCheck.IsChecked == true;
        style.ReduceMotion = ReduceMotionCheck.IsChecked == true;
        ApplySpecializedInspector(style);
        RefreshStyleView(style);
        MarkDirty($"已更新 {style.Type} / {style.Variant}");
    }

    private void ApplySpecializedInspector(StyleDefinition style)
    {
        if (style.Type is "window" or "dialog")
        {
            style.Window.Title = WindowTitleBox.Text.Trim().Length == 0 ? "Shinkou 窗口" : WindowTitleBox.Text.Trim();
            style.Window.TitleBarHeight = BoundedNumber(WindowTitleBarHeightBox.Text, style.Window.TitleBarHeight, 20, 96);
            style.Window.BackdropOpacity = Number(WindowBackdropOpacityBox.Text, style.Window.BackdropOpacity);
            style.Window.Chrome = (WindowChromeBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "macos";
            style.Window.Modal = WindowModalCheck.IsChecked == true;
            style.Window.Resizable = WindowResizableCheck.IsChecked == true;
            style.Window.Draggable = WindowDraggableCheck.IsChecked == true;
            style.Window.ShowControls = WindowControlsCheck.IsChecked == true;
        }
        else if (style.Type is "layout" or "split-view")
        {
            style.Layout.Mode = (LayoutModeBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "stack";
            style.Layout.Orientation = (LayoutOrientationBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "vertical";
            style.Layout.Alignment = (LayoutAlignmentBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "stretch";
            style.Layout.Gap = OptionalNumber(LayoutGapBox.Text, style.Layout.Gap);
            style.Layout.Columns = BoundedNumber(LayoutColumnsBox.Text, style.Layout.Columns, 1, 12);
            style.Layout.SplitRatio = Number(LayoutSplitRatioBox.Text, style.Layout.SplitRatio);
            style.Layout.ClipContent = LayoutClipCheck.IsChecked == true;
            style.Layout.ShowDividers = LayoutDividersCheck.IsChecked == true;
        }
        else if (style.Type is "file-view")
        {
            style.FileView.ViewMode = (FileViewModeBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "details";
            style.FileView.SortBy = (FileSortBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "name";
            style.FileView.SortDirection = (FileSortDirectionBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "asc";
            style.FileView.IconSize = BoundedNumber(FileIconSizeBox.Text, style.FileView.IconSize, 12, 96);
            style.FileView.RowHeight = BoundedNumber(FileRowHeightBox.Text, style.FileView.RowHeight, 20, 160);
            style.FileView.ShowThumbnails = FileThumbnailsCheck.IsChecked == true;
            style.FileView.ShowExtensions = FileExtensionsCheck.IsChecked == true;
            style.FileView.ShowHidden = FileHiddenCheck.IsChecked == true;
            style.FileView.AlternatingRows = FileAlternatingCheck.IsChecked == true;
            style.FileView.GroupByType = FileGroupingCheck.IsChecked == true;
        }
    }

    private void ColorPicker_OnChanged(object? sender, EventArgs e)
    {
        if (!_ready || _loading || Current is null) return;
        CaptureHistory();
        var style = Current;
        style.Background = BackgroundPicker.Color.ToString();
        style.Foreground = ForegroundPicker.Color.ToString();
        style.Border = BorderPicker.Color.ToString();
        style.HoverBackground = HoverPicker.Color.ToString();
        style.PressedBackground = PressedPicker.Color.ToString();
        RefreshStyleView(style);
        MarkDirty($"已更新颜色：{style.Type} / {style.Variant}");
    }

    private void AnimationValue_OnChanged(object sender, RoutedEventArgs e) => ApplyAnimationInspector();
    private void AnimationValue_OnChanged(object sender, TextChangedEventArgs e) => ApplyAnimationInspector();
    private void AnimationValue_OnChanged(object sender, SelectionChangedEventArgs e) => ApplyAnimationInspector();
    private void AnimationValue_OnChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => ApplyAnimationInspector();

    private void ApplyAnimationInspector()
    {
        var style = Current;
        if (!_ready || _loading || style is null) return;
        CaptureHistory();
        var animation = style.Animation;
        animation.Enabled = AnimationEnabledCheck.IsChecked == true;
        animation.DurationMs = AnimationDurationSlider.Value;
        animation.DelayMs = AnimationDelaySlider.Value;
        animation.Easing = (AnimationEasingBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "ease-out";
        animation.Repeat = (AnimationRepeatBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "once";
        animation.P1X = Number(BezierP1XBox.Text, animation.P1X);
        animation.P1Y = Number(BezierP1YBox.Text, animation.P1Y);
        animation.P2X = Number(BezierP2XBox.Text, animation.P2X);
        animation.P2Y = Number(BezierP2YBox.Text, animation.P2Y);
        animation.TriggerHover = TriggerHoverCheck.IsChecked == true;
        animation.TriggerPressed = TriggerPressedCheck.IsChecked == true;
        animation.TriggerFocus = TriggerFocusCheck.IsChecked == true;
        animation.TriggerDisabled = TriggerDisabledCheck.IsChecked == true;
        AnimationDurationValue.Text = $"{animation.DurationMs:0} ms";
        AnimationDelayValue.Text = $"{animation.DelayMs:0} ms";
        MarkDirty($"已更新动画：{style.Type} / {style.Variant}");
    }

    private void NewStyle_OnClick(object sender, RoutedEventArgs e)
    {
        CaptureHistory();
        var type = (NewStyleTypeBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "button";
        var used = _styles.Where(item => item.Type == type).Select(item => item.Variant).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var variant = "custom";
        var index = 2;
        while (used.Contains(variant)) variant = $"custom-{index++}";
        var style = new StyleDefinition { Type = type, Variant = variant, Animation = NewAnimation(type is "button" or "slider") };
        _styles.Add(style);
        StyleList.SelectedItem = style;
        MarkDirty($"已新建 {style.DisplayName}");
        _historyCaptured = false;
    }

    private void DuplicateStyle_OnClick(object sender, RoutedEventArgs e)
    {
        if (Current is null) return;
        CaptureHistory();
        var copy = Current.Clone();
        copy.Variant = $"{copy.Variant}-copy";
        _styles.Add(copy);
        StyleList.SelectedItem = copy;
        MarkDirty($"已复制 {copy.DisplayName}");
        _historyCaptured = false;
    }

    private void DeleteStyle_OnClick(object sender, RoutedEventArgs e)
    {
        if (Current is null || _styles.Count <= 1) return;
        CaptureHistory();
        var index = StyleList.SelectedIndex;
        var name = Current.DisplayName;
        _styles.Remove(Current);
        StyleList.SelectedIndex = Math.Clamp(index - 1, 0, _styles.Count - 1);
        MarkDirty($"已删除 {name}");
        _historyCaptured = false;
    }

    private void Undo_OnClick(object sender, RoutedEventArgs e)
    {
        if (_undo.Count == 0) { StatusText.Text = "没有可撤销的样式修改"; return; }
        _redo.Push(Snapshot());
        Restore(_undo.Pop());
        StatusText.Text = "已撤销样式修改";
    }

    private void Redo_OnClick(object sender, RoutedEventArgs e)
    {
        if (_redo.Count == 0) { StatusText.Text = "没有可重做的样式修改"; return; }
        _undo.Push(Snapshot());
        Restore(_redo.Pop());
        StatusText.Text = "已重做样式修改";
    }

    private void Restore(IReadOnlyList<StyleDefinition> state)
    {
        var selectedName = Current?.DisplayName;
        _loading = true;
        try
        {
            _styles.Clear();
            foreach (var item in state) _styles.Add(item.Clone());
            StyleList.SelectedIndex = Math.Max(0, _styles.ToList().FindIndex(item => item.DisplayName == selectedName));
        }
        finally { _loading = false; }
        _historyCaptured = false;
        DocumentTitleText.Text = "ShinkouUI Style Studio · 未保存";
        UpdateInspector();
        RenderPreview();
    }

    private void RefreshStyleView(StyleDefinition style)
    {
        StyleList.Items.Refresh();
        InspectorCaption.Text = $"{style.Type} / {style.Variant}";
        PreviewCaption.Text = InspectorCaption.Text;
        RenderPreview();
    }

    private void RenderPreview()
    {
        var style = Current;
        if (!_ready || style is null || PreviewHost is null) return;
        ApplyEditorTheme();
        PreviewHost.Children.Clear();
        var state = (PreviewStateBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "normal";
        var background = state switch { "hover" => style.HoverBackground, "pressed" => style.PressedBackground, _ => style.Background };
        var minWidth = style.MinWidth > 0 ? style.MinWidth : style.Width > 0 ? 0 : 300;
        var maxWidth = style.MaxWidth > 0 ? Math.Max(style.MaxWidth, minWidth) : style.Width > 0 ? double.PositiveInfinity : 540;
        var minHeight = style.MinHeight > 0 ? style.MinHeight : style.Height > 0 ? 0 : 86;
        var maxHeight = style.MaxHeight > 0 ? Math.Max(style.MaxHeight, minHeight) : style.Height > 0 ? double.PositiveInfinity : 220;
        var frame = new Border
        {
            MinWidth = minWidth, MinHeight = minHeight, MaxWidth = maxWidth, MaxHeight = maxHeight,
            Width = style.Width > 0 ? style.Width : double.NaN,
            Height = style.Height > 0 ? style.Height : double.NaN,
            Margin = style.Margin.ToThickness(), Padding = style.Padding.ToThickness(), Background = BrushFrom(background, style.Background),
            BorderBrush = BrushFrom(style.Border, "#DDE1E7"), BorderThickness = style.BorderThickness.ToThickness(),
            CornerRadius = new CornerRadius(style.Radius), HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center, RenderTransformOrigin = new Point(0.5, 0.5),
            RenderTransform = new ScaleTransform(1, 1)
        };
        if (style.Shadow) frame.Effect = new DropShadowEffect { BlurRadius = 18, ShadowDepth = 4, Opacity = 0.14, Color = Colors.Black };
        var content = CreatePreviewControl(style, state);
        frame.Child = content;
        PreviewHost.Children.Add(frame);
        _previewFrame = frame;
        _previewContent = content;
        if (state == "disabled") { frame.Opacity = 0.48; content.IsEnabled = false; }
        PlayAnimationButton.IsEnabled = style.Animation.Enabled && style.Animation.DurationMs > 0;
    }

    private FrameworkElement CreatePreviewControl(StyleDefinition style, string state)
    {
        var foreground = BrushFrom(style.Foreground, "#20242B");
        var font = SafeFont(style.FontFamily);
        var weight = style.FontWeightName switch { "Bold" => FontWeights.Bold, "SemiBold" => FontWeights.SemiBold, _ => FontWeights.Normal };
        switch (style.Type.ToLowerInvariant())
        {
            case "window":
            case "dialog":
                return CreateWindowPreview(style, foreground, font, weight);
            case "layout":
            case "split-view":
                return CreateLayoutPreview(style, foreground, font);
            case "file-view":
                return CreateFileViewPreview(style, foreground, font);
            case "button":
                var button = new Button { Content = "示例按钮", Background = Brushes.Transparent, BorderThickness = new Thickness(0), Foreground = foreground, FontFamily = font, FontSize = style.FontSize, FontWeight = weight, Padding = style.Padding.ToThickness(), HorizontalContentAlignment = HorizontalAlignment.Center };
                button.Template = CreatePreviewButtonTemplate();
                button.Click += (_, _) => StatusText.Text = "预览按钮已点击";
                return button;
            case "textbox":
                return new TextBox { Text = "示例文本", Background = Brushes.Transparent, BorderThickness = new Thickness(0), Foreground = foreground, FontFamily = font, FontSize = style.FontSize, Padding = style.Padding.ToThickness(), MinWidth = 260 };
            case "checkbox":
                return new CheckBox { Content = "启用状态", IsChecked = true, Foreground = foreground, FontFamily = font, FontSize = style.FontSize, VerticalContentAlignment = VerticalAlignment.Center };
            case "slider":
                return new Slider { Minimum = 0, Maximum = 100, Value = 55, Foreground = BrushFrom(style.Background, "#0A84FF"), MinWidth = 280 };
            case "progress":
                return new ProgressBar { Minimum = 0, Maximum = 100, Value = 64, Foreground = BrushFrom(style.Background, "#0A84FF"), Background = BrushFrom(style.Border, "#DDE1E7"), MinWidth = 280, Height = 8 };
            case "richtextbox":
                var rich = new RichTextBox { Background = Brushes.Transparent, BorderThickness = new Thickness(0), Foreground = foreground, FontFamily = font, FontSize = style.FontSize, Padding = style.Padding.ToThickness() };
                rich.Document.Blocks.Add(new Paragraph(new Run("示例富文本内容")));
                return rich;
            case "panel":
                return new TextBlock { Text = "示例面板内容", Foreground = foreground, FontFamily = font, FontSize = style.FontSize, FontWeight = weight, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            default: return new TextBlock { Text = "示例控件", Foreground = foreground, FontFamily = font, FontSize = style.FontSize };
        }
    }

    private FrameworkElement CreateWindowPreview(StyleDefinition style, Brush foreground, FontFamily font, FontWeight weight)
    {
        var root = new Grid { MinWidth = 420, MinHeight = 230, ClipToBounds = true };
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(style.Window.TitleBarHeight) });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        var chrome = new Border
        {
            Background = BrushFrom(style.Window.Chrome == "windows" ? "#EEF1F5" : style.Background, style.Background),
            BorderBrush = BrushFrom(style.Border, "#DDE1E7"), BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(12, 0, 12, 0)
        };
        var titleBar = new Grid();
        titleBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        titleBar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        titleBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        if (style.Window.ShowControls && style.Window.Chrome != "minimal")
        {
            var controls = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            controls.Children.Add(CreateWindowDot("#FF5F57"));
            controls.Children.Add(CreateWindowDot("#FFBD2E"));
            controls.Children.Add(CreateWindowDot("#28C840"));
            Grid.SetColumn(controls, 0);
            titleBar.Children.Add(controls);
        }
        var title = new TextBlock { Text = style.Window.Title, Foreground = foreground, FontFamily = font, FontWeight = weight, FontSize = 12, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center };
        Grid.SetColumn(title, 1);
        titleBar.Children.Add(title);
        if (style.Window.Modal)
        {
            var modal = new TextBlock { Text = "MODAL", Foreground = BrushFrom(style.HoverBackground, "#0A84FF"), FontSize = 9, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(modal, 2);
            titleBar.Children.Add(modal);
        }
        chrome.Child = titleBar;
        Grid.SetRow(chrome, 0);
        root.Children.Add(chrome);

        var body = new Border { Background = BrushFrom(style.Background, "#FFFFFF"), Padding = style.Padding.ToThickness() };
        var bodyGrid = new Grid();
        var content = new StackPanel { VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center };
        content.Children.Add(new TextBlock { Text = "子窗口内容区域", Foreground = foreground, FontFamily = font, FontSize = style.FontSize, FontWeight = weight, HorizontalAlignment = HorizontalAlignment.Center });
        content.Children.Add(new TextBlock { Text = style.Window.Draggable ? "可拖动标题栏" : "固定标题栏", Foreground = BrushFrom(style.HoverBackground, "#86868B"), FontSize = 10, Margin = new Thickness(0, 8, 0, 0), HorizontalAlignment = HorizontalAlignment.Center });
        if (style.Window.Resizable)
            content.Children.Add(new TextBlock { Text = "↘ 可调整大小", Foreground = BrushFrom(style.HoverBackground, "#86868B"), FontSize = 9, Margin = new Thickness(0, 5, 0, 0), HorizontalAlignment = HorizontalAlignment.Center });
        bodyGrid.Children.Add(content);
        body.Child = bodyGrid;
        Grid.SetRow(body, 1);
        root.Children.Add(body);
        return root;
    }

    private static Ellipse CreateWindowDot(string color) => new()
    {
        Width = 10, Height = 10, Fill = BrushFrom(color, color), Margin = new Thickness(0, 0, 6, 0)
    };

    private FrameworkElement CreateLayoutPreview(StyleDefinition style, Brush foreground, FontFamily font)
    {
        var layout = style.Layout;
        var root = new Grid { MinWidth = 420, MinHeight = 210, ClipToBounds = layout.ClipContent };
        var first = CreateLayoutCell("导航", foreground, font);
        var second = CreateLayoutCell("主内容", foreground, font);
        var third = CreateLayoutCell("检查器", foreground, font);
        if (layout.Mode == "split")
        {
            root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(Math.Clamp(layout.SplitRatio, 0.1, 0.8), GridUnitType.Star) });
            root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(layout.ShowDividers ? layout.Gap : Math.Max(0, layout.Gap), GridUnitType.Pixel) });
            root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Grid.SetColumn(first, 0);
            Grid.SetColumn(second, 2);
            root.Children.Add(first);
            root.Children.Add(second);
            if (layout.ShowDividers)
            {
                var divider = new Border { Background = BrushFrom(style.Border, "#DDE1E7"), Width = 1, HorizontalAlignment = HorizontalAlignment.Center };
                Grid.SetColumn(divider, 1);
                root.Children.Add(divider);
            }
            return root;
        }

        if (layout.Mode == "grid")
        {
            var columns = Math.Max(1, (int)Math.Round(layout.Columns));
            var grid = new UniformGrid { Columns = columns, HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch };
            grid.Children.Add(first); grid.Children.Add(second); grid.Children.Add(third);
            root.Children.Add(grid);
            return root;
        }

        var stack = new StackPanel { Orientation = layout.Orientation == "horizontal" ? Orientation.Horizontal : Orientation.Vertical };
        foreach (var cell in new[] { first, second, third })
        {
            if (stack.Children.Count > 0)
                cell.Margin = layout.Orientation == "horizontal" ? new Thickness(layout.Gap, 0, 0, 0) : new Thickness(0, layout.Gap, 0, 0);
            stack.Children.Add(cell);
        }
        root.Children.Add(stack);
        return root;
    }

    private static Border CreateLayoutCell(string label, Brush foreground, FontFamily font) => new()
    {
        Background = new SolidColorBrush(Color.FromArgb(26, 10, 132, 255)), BorderBrush = new SolidColorBrush(Color.FromArgb(70, 10, 132, 255)),
        BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(6), Padding = new Thickness(14), Margin = new Thickness(0),
        Child = new TextBlock { Text = label, Foreground = foreground, FontFamily = font, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center }
    };

    private FrameworkElement CreateFileViewPreview(StyleDefinition style, Brush foreground, FontFamily font)
    {
        var file = style.FileView;
        var root = new Border { Background = Brushes.Transparent, MinWidth = 420, MinHeight = 230, Padding = new Thickness(2) };
        var content = file.ViewMode switch
        {
            "list" => CreateFileListView(style, foreground, font),
            "tiles" => CreateFileTilesView(style, foreground, font),
            "content" => CreateFileContentView(style, foreground, font),
            _ => CreateFileDetailsView(style, foreground, font)
        };
        root.Child = content;
        return root;
    }

    private FrameworkElement CreateFileDetailsView(StyleDefinition style, Brush foreground, FontFamily font)
    {
        var file = style.FileView;
        var grid = new Grid { ClipToBounds = true };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(2, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.3, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0.8, GridUnitType.Star) });
        var header = new Border { Background = BrushFrom(style.HoverBackground, "#F2F7FD"), Padding = new Thickness(9, 6, 9, 6) };
        var headerGrid = new Grid();
        AddFileColumns(headerGrid);
        AddHeaderCell(headerGrid, 0, "名称", foreground);
        AddHeaderCell(headerGrid, 1, "修改日期", foreground);
        AddHeaderCell(headerGrid, 2, "类型", foreground);
        AddHeaderCell(headerGrid, 3, "大小", foreground);
        header.Child = headerGrid;
        Grid.SetRow(header, 0);
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(file.RowHeight) });
        grid.Children.Add(header);
        var rows = new StackPanel { Margin = new Thickness(0, file.RowHeight, 0, 0) };
        var values = new[] { ("Assets", "今天", "文件夹", "—"), ("main.cpp", "昨天", "C++ 源文件", "18 KB"), ("theme.xml", "周一", "XML 文档", "7 KB"), ("preview.png", "上周", "PNG 图片", "320 KB") };
        for (var i = 0; i < values.Length; i++)
        {
            var row = CreateFileDetailsRow(style, foreground, font, values[i], i, file.RowHeight);
            rows.Children.Add(row);
        }
        grid.Children.Add(rows);
        return grid;
    }

    private FrameworkElement CreateFileListView(StyleDefinition style, Brush foreground, FontFamily font)
    {
        var list = new StackPanel { ClipToBounds = true };
        foreach (var (name, kind) in new[] { ("Assets", "文件夹"), ("main.cpp", "C++ 源文件"), ("theme.xml", "XML 文档"), ("preview.png", "PNG 图片") })
        {
            var row = new Border { Height = style.FileView.RowHeight, Background = Brushes.Transparent, Padding = new Thickness(10, 0, 10, 0) };
            var line = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            line.Children.Add(CreateFileGlyph(style.FileView.IconSize, name.EndsWith("Assets"), foreground));
            line.Children.Add(new TextBlock { Text = style.FileView.ShowExtensions ? name : System.IO.Path.GetFileNameWithoutExtension(name), Foreground = foreground, FontFamily = font, FontSize = style.FontSize, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 0, 0) });
            line.Children.Add(new TextBlock { Text = $"  {kind}", Foreground = BrushFrom(style.HoverBackground, "#86868B"), FontSize = 10, VerticalAlignment = VerticalAlignment.Center });
            row.Child = line;
            list.Children.Add(row);
        }
        return list;
    }

    private FrameworkElement CreateFileTilesView(StyleDefinition style, Brush foreground, FontFamily font)
    {
        var grid = new UniformGrid { Columns = 3, Rows = 2, ClipToBounds = true };
        foreach (var name in new[] { "Assets", "main.cpp", "theme.xml", "preview.png", "editor.xaml", "README" })
        {
            var tile = new Border { Background = Brushes.Transparent, BorderBrush = BrushFrom(style.Border, "#DDE1E7"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(style.Radius), Height = style.FileView.RowHeight, Margin = new Thickness(4), Padding = new Thickness(8) };
            var stack = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            stack.Children.Add(CreateFileGlyph(style.FileView.IconSize, name == "Assets", foreground));
            stack.Children.Add(new TextBlock { Text = style.FileView.ShowExtensions ? name : System.IO.Path.GetFileNameWithoutExtension(name), Foreground = foreground, FontFamily = font, FontSize = 10, TextTrimming = TextTrimming.CharacterEllipsis, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 6, 0, 0) });
            tile.Child = stack;
            grid.Children.Add(tile);
        }
        return grid;
    }

    private FrameworkElement CreateFileContentView(StyleDefinition style, Brush foreground, FontFamily font)
    {
        var list = new StackPanel { ClipToBounds = true };
        foreach (var (name, detail) in new[] { ("Assets", "文件夹 · 今天"), ("main.cpp", "C++ 源文件 · 18 KB"), ("preview.png", "PNG 图片 · 320 KB") })
        {
            var row = new Border { Height = style.FileView.RowHeight, Background = Brushes.Transparent, Padding = new Thickness(10, 4, 10, 4) };
            var line = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            line.Children.Add(CreateFileGlyph(style.FileView.IconSize, name == "Assets", foreground));
            var text = new StackPanel { Margin = new Thickness(12, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(new TextBlock { Text = style.FileView.ShowExtensions ? name : System.IO.Path.GetFileNameWithoutExtension(name), Foreground = foreground, FontFamily = font, FontSize = style.FontSize, TextTrimming = TextTrimming.CharacterEllipsis });
            text.Children.Add(new TextBlock { Text = detail, Foreground = BrushFrom(style.HoverBackground, "#86868B"), FontSize = 10, Margin = new Thickness(0, 4, 0, 0), TextTrimming = TextTrimming.CharacterEllipsis });
            line.Children.Add(text);
            row.Child = line;
            list.Children.Add(row);
        }
        return list;
    }

    private static void AddFileColumns(Grid grid)
    {
        while (grid.ColumnDefinitions.Count < 4)
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
    }

    private static void AddHeaderCell(Grid grid, int column, string text, Brush foreground)
    {
        var cell = new TextBlock { Text = text, Foreground = foreground, FontSize = 10, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        Grid.SetColumn(cell, column);
        grid.Children.Add(cell);
    }

    private static Border CreateFileDetailsRow(StyleDefinition style, Brush foreground, FontFamily font, (string Name, string Date, string Type, string Size) value, int index, double height)
    {
        var row = new Border { Height = height, Background = style.FileView.AlternatingRows && index % 2 == 1 ? BrushFrom(style.HoverBackground, "#F7F9FC") : Brushes.Transparent, Padding = new Thickness(9, 0, 9, 0) };
        var grid = new Grid();
        AddFileColumns(grid);
        AddFileCell(grid, 0, value.Name, foreground, font, true);
        AddFileCell(grid, 1, value.Date, foreground, font, false);
        AddFileCell(grid, 2, value.Type, foreground, font, false);
        AddFileCell(grid, 3, value.Size, foreground, font, false);
        row.Child = grid;
        return row;
    }

    private static void AddFileCell(Grid grid, int column, string text, Brush foreground, FontFamily font, bool withIcon)
    {
        var line = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        if (withIcon) line.Children.Add(CreateFileGlyph(18, text == "Assets", foreground));
        line.Children.Add(new TextBlock { Text = text, Foreground = foreground, FontFamily = font, FontSize = 10, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(withIcon ? 7 : 0, 0, 0, 0) });
        Grid.SetColumn(line, column);
        grid.Children.Add(line);
    }

    private static Border CreateFileGlyph(double size, bool folder, Brush foreground) => new()
    {
        Width = size, Height = size, CornerRadius = new CornerRadius(4), Background = new SolidColorBrush(Color.FromArgb(32, 10, 132, 255)),
        Child = new TextBlock { Text = folder ? "▰" : "▧", Foreground = foreground, FontSize = Math.Max(10, size * 0.62), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center }
    };

    private static ControlTemplate CreatePreviewButtonTemplate()
    {
        var chrome = new FrameworkElementFactory(typeof(Border));
        chrome.SetValue(Border.BackgroundProperty, new TemplateBindingExtension(Control.BackgroundProperty));
        chrome.SetValue(Border.BorderBrushProperty, new TemplateBindingExtension(Control.BorderBrushProperty));
        chrome.SetValue(Border.BorderThicknessProperty, new TemplateBindingExtension(Control.BorderThicknessProperty));
        chrome.SetValue(Border.PaddingProperty, new TemplateBindingExtension(Control.PaddingProperty));
        var presenter = new FrameworkElementFactory(typeof(ContentPresenter));
        presenter.SetValue(ContentPresenter.ContentSourceProperty, "Content");
        presenter.SetValue(FrameworkElement.HorizontalAlignmentProperty, new TemplateBindingExtension(Control.HorizontalContentAlignmentProperty));
        presenter.SetValue(FrameworkElement.VerticalAlignmentProperty, new TemplateBindingExtension(Control.VerticalContentAlignmentProperty));
        chrome.AppendChild(presenter);
        return new ControlTemplate(typeof(Button)) { VisualTree = chrome };
    }

    private void PlayAnimation_OnClick(object sender, RoutedEventArgs e)
    {
        var style = Current;
        var frame = _previewFrame;
        if (style is null || frame is null || !style.Animation.Enabled || style.Animation.DurationMs <= 0)
        {
            StatusText.Text = "当前变体未启用动画";
            return;
        }

        var animation = style.Animation;
        var duration = new Duration(TimeSpan.FromMilliseconds(animation.DurationMs));
        var delay = TimeSpan.FromMilliseconds(animation.DelayMs);
        var toColor = ParseColor(style.HoverBackground, Colors.White);
        if (frame.Background is not SolidColorBrush brush) return;
        var colorAnimation = new ColorAnimation(brush.Color, toColor, duration)
        {
            BeginTime = delay,
            AutoReverse = animation.Repeat == "ping-pong",
            EasingFunction = CreateEasing(animation)
        };
        colorAnimation.RepeatBehavior = animation.Repeat == "loop" ? new RepeatBehavior(2) : new RepeatBehavior(1);
        brush.BeginAnimation(SolidColorBrush.ColorProperty, colorAnimation);

        if (frame.RenderTransform is ScaleTransform scale)
        {
            var scaleAnimation = new DoubleAnimation(1, 1.035, duration)
            {
                BeginTime = delay, AutoReverse = true, EasingFunction = CreateEasing(animation),
                RepeatBehavior = animation.Repeat == "loop" ? new RepeatBehavior(2) : new RepeatBehavior(1)
            };
            scale.BeginAnimation(ScaleTransform.ScaleXProperty, scaleAnimation);
            scale.BeginAnimation(ScaleTransform.ScaleYProperty, scaleAnimation);
        }
        StatusText.Text = $"正在播放 {animation.Easing} 动画 · {animation.DurationMs:0} ms";
    }

    private static IEasingFunction CreateEasing(AnimationDefinition animation) => animation.Easing switch
    {
        "linear" => null!,
        "ease-in" => new CubicEase { EasingMode = EasingMode.EaseIn },
        "ease-in-out" => new CubicEase { EasingMode = EasingMode.EaseInOut },
        "cubic-bezier" => new CubicBezierEasing(animation.P1X, animation.P1Y, animation.P2X, animation.P2Y),
        "spring" => new BackEase { Amplitude = 0.35, EasingMode = EasingMode.EaseOut },
        _ => new CubicEase { EasingMode = EasingMode.EaseOut }
    };

    private void ApplyEditorTheme()
    {
        var dark = (ThemeBox.SelectedItem as ComboBoxItem)?.Tag as string == "dark";
        SetThemeBrush("WindowBrush", dark ? "#1C1C1E" : "#F5F5F7");
        SetThemeBrush("ToolbarBrush", dark ? "#242426" : "#FAFAFB");
        SetThemeBrush("SidebarBrush", dark ? "#202022" : "#F0F1F3");
        SetThemeBrush("SurfaceBrush", dark ? "#242426" : "#FFFFFF");
        SetThemeBrush("CanvasBrush", dark ? "#303034" : "#EBECEF");
        SetThemeBrush("LineBrush", dark ? "#414146" : "#D9DCE2");
        SetThemeBrush("TextBrush", dark ? "#F5F5F7" : "#1D1D1F");
        SetThemeBrush("MutedBrush", dark ? "#A1A1A6" : "#86868B");
        RootFrame.Background = BrushFrom(dark ? "#1C1C1E" : "#F5F5F7", "#F5F5F7");
        ToolbarFrame.Background = BrushFrom(dark ? "#242426" : "#FAFAFB", "#FAFAFB");
        SidebarFrame.Background = BrushFrom(dark ? "#202022" : "#F0F1F3", "#F0F1F3");
        InspectorFrame.Background = BrushFrom(dark ? "#242426" : "#FFFFFF", "#FFFFFF");
        FooterFrame.Background = BrushFrom(dark ? "#202022" : "#F0F1F3", "#F0F1F3");
        PreviewSurface.Background = BrushFrom(dark ? "#303034" : "#EBECEF", "#EBECEF");
        var canvasFrame = FindVisualChildren<Border>(this).FirstOrDefault(border => border.Padding.Left is >= 23 and <= 25 && border.Padding.Top is >= 23 and <= 25);
        if (canvasFrame is not null)
            canvasFrame.Background = BrushFrom(dark ? "#303034" : "#EBECEF", "#EBECEF");
        BackgroundPicker.IsDark = dark;
        ForegroundPicker.IsDark = dark;
        BorderPicker.IsDark = dark;
        HoverPicker.IsDark = dark;
        PressedPicker.IsDark = dark;
        foreach (var control in FindVisualChildren<Control>(this))
        {
            if (control is TextBox or ComboBox)
            {
                control.Background = BrushFrom(dark ? "#303034" : "#F8F8FA", "#F8F8FA");
                control.BorderBrush = BrushFrom(dark ? "#4A4A4F" : "#D9DCE2", "#D9DCE2");
                control.Foreground = BrushFrom(dark ? "#F5F5F7" : "#1D1D1F", "#1D1D1F");
            }
            else if (control is CheckBox)
            {
                control.Foreground = BrushFrom(dark ? "#F5F5F7" : "#1D1D1F", "#1D1D1F");
            }
            else if (control is Button button && IsTransparent(button.Background))
            {
                button.Foreground = BrushFrom(dark ? "#F5F5F7" : "#1D1D1F", "#1D1D1F");
            }
        }
        foreach (var textBlock in FindVisualChildren<TextBlock>(this))
            textBlock.Foreground = BrushFrom(dark ? "#F5F5F7" : "#1D1D1F", "#1D1D1F");
        foreach (var item in FindVisualChildren<ListBoxItem>(StyleList))
            item.Background = item.IsSelected ? BrushFrom(dark ? "#3A3A3C" : "#DDEBFA", "#DDEBFA") : Brushes.Transparent;
        PreviewCaption.Foreground = BrushFrom(dark ? "#A1A1A6" : "#86868B", "#86868B");
        InspectorCaption.Foreground = BrushFrom(dark ? "#A1A1A6" : "#86868B", "#86868B");
        StatusText.Foreground = BrushFrom(dark ? "#A1A1A6" : "#86868B", "#86868B");
    }

    private void SetThemeBrush(string key, string value)
    {
        Resources[key] = new SolidColorBrush(ParseColor(value, Colors.Transparent));
    }

    private static bool IsTransparent(Brush? brush) => brush is null || brush == Brushes.Transparent || (brush is SolidColorBrush solid && solid.Color.A == 0);

    private static IEnumerable<T> FindVisualChildren<T>(DependencyObject root) where T : DependencyObject
    {
        if (root is null) yield break;
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is T match) yield return match;
            foreach (var descendant in FindVisualChildren<T>(child)) yield return descendant;
        }
    }

    private void PreviewState_OnChanged(object sender, SelectionChangedEventArgs e) { if (_ready && !_loading) RenderPreview(); }
    private void Theme_OnChanged(object sender, SelectionChangedEventArgs e) { if (_ready && !_loading) RenderPreview(); }

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.S && Keyboard.Modifiers.HasFlag(ModifierKeys.Control)) { Save_OnClick(sender, e); e.Handled = true; }
        else if (e.Key == Key.Z && Keyboard.Modifiers.HasFlag(ModifierKeys.Control)) { Undo_OnClick(sender, e); e.Handled = true; }
        else if (e.Key == Key.Y && Keyboard.Modifiers.HasFlag(ModifierKeys.Control)) { Redo_OnClick(sender, e); e.Handled = true; }
    }

    private void Save_OnClick(object sender, RoutedEventArgs e)
    {
        var dialog = new SaveFileDialog { Filter = "Shinkou UI Style XML (*.xml)|*.xml|所有文件|*.*", DefaultExt = ".xml" };
        if (dialog.ShowDialog() != true) return;
        var theme = new XElement("theme", new XAttribute("id", ((ThemeBox.SelectedItem as ComboBoxItem)?.Tag as string == "dark") ? "dark" : "light"), new XAttribute("font-family", "Microsoft YaHei"), new XElement("styles", _styles.Select(ToXml)));
        new XDocument(new XElement("shinkou-ui-styles", new XAttribute("version", "5"), theme)).Save(dialog.FileName);
        DocumentTitleText.Text = "ShinkouUI Style Studio";
        StatusText.Text = $"已保存：{dialog.FileName}";
        _historyCaptured = false;
    }

    private static XElement ToXml(StyleDefinition style) => new("style",
        new XAttribute("type", style.Type), new XAttribute("variant", style.Variant), new XAttribute("background", style.Background),
        new XAttribute("foreground", style.Foreground), new XAttribute("border", style.Border), new XAttribute("hover", style.HoverBackground),
        new XAttribute("pressed", style.PressedBackground), new XAttribute("radius", style.Radius.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("padding", LegacyEdgeValue(style.Padding)), new XAttribute("margin", LegacyEdgeValue(style.Margin)),
        new XAttribute("font-size", style.FontSize.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("font-family", style.FontFamily), new XAttribute("font-weight", style.FontWeightName),
        new XAttribute("border-thickness", LegacyEdgeValue(style.BorderThickness)), new XAttribute("width", OptionalNumber(style.Width)),
        new XAttribute("height", OptionalNumber(style.Height)), new XAttribute("min-width", OptionalNumber(style.MinWidth)),
        new XAttribute("max-width", OptionalNumber(style.MaxWidth)), new XAttribute("min-height", OptionalNumber(style.MinHeight)),
        new XAttribute("max-height", OptionalNumber(style.MaxHeight)), new XAttribute("shadow", style.Shadow),
        new XAttribute("reduce-motion", style.ReduceMotion), EdgesXml("margin", style.Margin), EdgesXml("padding", style.Padding),
        EdgesXml("border-thickness", style.BorderThickness), WindowXml(style.Window), LayoutXml(style.Layout), FileViewXml(style.FileView), new XElement("animation",
            new XAttribute("enabled", style.Animation.Enabled), new XAttribute("duration-ms", style.Animation.DurationMs.ToString(CultureInfo.InvariantCulture)),
            new XAttribute("delay-ms", style.Animation.DelayMs.ToString(CultureInfo.InvariantCulture)), new XAttribute("easing", style.Animation.Easing),
            new XAttribute("repeat", style.Animation.Repeat), new XAttribute("p1x", style.Animation.P1X.ToString(CultureInfo.InvariantCulture)),
            new XAttribute("p1y", style.Animation.P1Y.ToString(CultureInfo.InvariantCulture)), new XAttribute("p2x", style.Animation.P2X.ToString(CultureInfo.InvariantCulture)),
            new XAttribute("p2y", style.Animation.P2Y.ToString(CultureInfo.InvariantCulture)), new XAttribute("hover", style.Animation.TriggerHover),
            new XAttribute("pressed", style.Animation.TriggerPressed), new XAttribute("focus", style.Animation.TriggerFocus),
            new XAttribute("disabled", style.Animation.TriggerDisabled)));

    private void Load_OnClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "Shinkou UI Style XML (*.xml)|*.xml|所有文件|*.*" };
        if (dialog.ShowDialog() != true) return;
        try
        {
            var root = XDocument.Load(dialog.FileName).Root ?? throw new InvalidDataException("缺少根节点");
            var theme = root.Element("theme");
            var styles = theme?.Element("styles")?.Elements("style").Select(FromXml).ToList() ?? throw new InvalidDataException("缺少 styles 节点");
            if (styles.Count == 0) throw new InvalidDataException("样式列表为空");
            _loading = true;
            _styles.Clear();
            foreach (var style in styles) _styles.Add(style);
            if (theme?.Attribute("id")?.Value == "dark") ThemeBox.SelectedItem = ThemeBox.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Tag == "dark");
            else ThemeBox.SelectedItem = ThemeBox.Items.OfType<ComboBoxItem>().FirstOrDefault(item => (string?)item.Tag == "light");
            StyleList.SelectedIndex = 0;
            _loading = false;
            _undo.Clear(); _redo.Clear(); _historyCaptured = false;
            DocumentTitleText.Text = "ShinkouUI Style Studio";
            UpdateInspector(); ApplyEditorTheme(); RenderPreview();
            StatusText.Text = $"已加载：{dialog.FileName}";
        }
        catch (Exception exception) { _loading = false; StatusText.Text = $"加载失败：{exception.Message}"; }
    }

    private static StyleDefinition FromXml(XElement element)
    {
        static double Number(XElement node, string name, double fallback) => double.TryParse((string?)node.Attribute(name), NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? value : fallback;
        static EdgeValues Edges(XElement node, string name, double fallback)
        {
            var child = node.Element(name);
            if (child is null)
            {
                var legacy = Number(node, name, fallback);
                return new EdgeValues(legacy);
            }

            return new EdgeValues
            {
                Top = Number(child, "top", fallback), Right = Number(child, "right", fallback),
                Bottom = Number(child, "bottom", fallback), Left = Number(child, "left", fallback)
            };
        }

        var animationNode = element.Element("animation");
        var animation = new AnimationDefinition
        {
            Enabled = string.Equals((string?)animationNode?.Attribute("enabled"), "true", StringComparison.OrdinalIgnoreCase),
            DurationMs = Number(animationNode ?? element, "duration-ms", 180), DelayMs = Number(animationNode ?? element, "delay-ms", 0),
            Easing = (string?)animationNode?.Attribute("easing") ?? "ease-out", Repeat = (string?)animationNode?.Attribute("repeat") ?? "once",
            P1X = Number(animationNode ?? element, "p1x", 0.25), P1Y = Number(animationNode ?? element, "p1y", 0.1),
            P2X = Number(animationNode ?? element, "p2x", 0.25), P2Y = Number(animationNode ?? element, "p2y", 1),
            TriggerHover = !string.Equals((string?)animationNode?.Attribute("hover"), "false", StringComparison.OrdinalIgnoreCase),
            TriggerPressed = !string.Equals((string?)animationNode?.Attribute("pressed"), "false", StringComparison.OrdinalIgnoreCase),
            TriggerFocus = string.Equals((string?)animationNode?.Attribute("focus"), "true", StringComparison.OrdinalIgnoreCase),
            TriggerDisabled = string.Equals((string?)animationNode?.Attribute("disabled"), "true", StringComparison.OrdinalIgnoreCase)
        };
        return new StyleDefinition
        {
            Type = (string?)element.Attribute("type") ?? "button", Variant = (string?)element.Attribute("variant") ?? "default",
            Background = (string?)element.Attribute("background") ?? "#F1F3F6", Foreground = (string?)element.Attribute("foreground") ?? "#20242B",
            Border = (string?)element.Attribute("border") ?? "#DDE1E7", HoverBackground = (string?)element.Attribute("hover") ?? "#E8EDF3", PressedBackground = (string?)element.Attribute("pressed") ?? "#D9E0E8",
            Radius = Number(element, "radius", 6), Margin = Edges(element, "margin", 0), Padding = Edges(element, "padding", 12), FontSize = Number(element, "font-size", 14),
            FontFamily = (string?)element.Attribute("font-family") ?? "Microsoft YaHei", FontWeightName = (string?)element.Attribute("font-weight") ?? "Regular",
            BorderThickness = Edges(element, "border-thickness", 1), Width = Number(element, "width", 0), Height = Number(element, "height", 0),
            MinWidth = Number(element, "min-width", 0), MaxWidth = Number(element, "max-width", 0), MinHeight = Number(element, "min-height", 0),
            MaxHeight = Number(element, "max-height", 0), Shadow = string.Equals((string?)element.Attribute("shadow"), "true", StringComparison.OrdinalIgnoreCase),
            Window = ReadWindow(element), Layout = ReadLayout(element), FileView = ReadFileView(element),
            ReduceMotion = string.Equals((string?)element.Attribute("reduce-motion"), "true", StringComparison.OrdinalIgnoreCase), Animation = animation
        };
    }

    private static Color ParseColor(string? value, Color fallback)
    {
        try { return (Color)ColorConverter.ConvertFromString(value ?? string.Empty)!; }
        catch { return fallback; }
    }

    private static Brush BrushFrom(string value, string fallback)
    {
        return new SolidColorBrush(ParseColor(value, ParseColor(fallback, Colors.Transparent)));
    }

    private static XElement EdgesXml(string name, EdgeValues edges) => new(name,
        new XAttribute("top", edges.Top.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("right", edges.Right.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("bottom", edges.Bottom.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("left", edges.Left.ToString(CultureInfo.InvariantCulture)));

    private static XElement WindowXml(WindowDefinition window) => new("window",
        new XAttribute("title", window.Title), new XAttribute("chrome", window.Chrome),
        new XAttribute("title-bar-height", window.TitleBarHeight.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("backdrop-opacity", window.BackdropOpacity.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("modal", window.Modal), new XAttribute("resizable", window.Resizable),
        new XAttribute("draggable", window.Draggable), new XAttribute("show-controls", window.ShowControls));

    private static XElement LayoutXml(LayoutDefinition layout) => new("layout",
        new XAttribute("mode", layout.Mode), new XAttribute("orientation", layout.Orientation),
        new XAttribute("alignment", layout.Alignment), new XAttribute("gap", layout.Gap.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("columns", layout.Columns.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("split-ratio", layout.SplitRatio.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("clip-content", layout.ClipContent), new XAttribute("show-dividers", layout.ShowDividers));

    private static XElement FileViewXml(FileViewDefinition file) => new("file-view",
        new XAttribute("mode", file.ViewMode), new XAttribute("sort-by", file.SortBy),
        new XAttribute("sort-direction", file.SortDirection), new XAttribute("icon-size", file.IconSize.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("row-height", file.RowHeight.ToString(CultureInfo.InvariantCulture)),
        new XAttribute("show-thumbnails", file.ShowThumbnails), new XAttribute("show-extensions", file.ShowExtensions),
        new XAttribute("show-hidden", file.ShowHidden), new XAttribute("alternating-rows", file.AlternatingRows),
        new XAttribute("group-by-type", file.GroupByType));

    private static WindowDefinition ReadWindow(XElement style)
    {
        var node = style.Element("window");
        return new WindowDefinition
        {
            Title = (string?)node?.Attribute("title") ?? "Shinkou 窗口", Chrome = (string?)node?.Attribute("chrome") ?? "macos",
            TitleBarHeight = AttributeNumber(node, "title-bar-height", 34), BackdropOpacity = AttributeNumber(node, "backdrop-opacity", 0.35),
            Modal = AttributeBool(node, "modal", false), Resizable = AttributeBool(node, "resizable", true),
            Draggable = AttributeBool(node, "draggable", true), ShowControls = AttributeBool(node, "show-controls", true)
        };
    }

    private static LayoutDefinition ReadLayout(XElement style)
    {
        var node = style.Element("layout");
        return new LayoutDefinition
        {
            Mode = (string?)node?.Attribute("mode") ?? "stack", Orientation = (string?)node?.Attribute("orientation") ?? "vertical",
            Alignment = (string?)node?.Attribute("alignment") ?? "stretch", Gap = AttributeNumber(node, "gap", 12),
            Columns = AttributeNumber(node, "columns", 2), SplitRatio = AttributeNumber(node, "split-ratio", 0.35),
            ClipContent = AttributeBool(node, "clip-content", true), ShowDividers = AttributeBool(node, "show-dividers", false)
        };
    }

    private static FileViewDefinition ReadFileView(XElement style)
    {
        var node = style.Element("file-view");
        return new FileViewDefinition
        {
            ViewMode = (string?)node?.Attribute("mode") ?? "details", SortBy = (string?)node?.Attribute("sort-by") ?? "name",
            SortDirection = (string?)node?.Attribute("sort-direction") ?? "asc", IconSize = AttributeNumber(node, "icon-size", 18),
            RowHeight = AttributeNumber(node, "row-height", 32), ShowThumbnails = AttributeBool(node, "show-thumbnails", true),
            ShowExtensions = AttributeBool(node, "show-extensions", true), ShowHidden = AttributeBool(node, "show-hidden", false),
            AlternatingRows = AttributeBool(node, "alternating-rows", true), GroupByType = AttributeBool(node, "group-by-type", false)
        };
    }

    private static double AttributeNumber(XElement? node, string name, double fallback)
        => double.TryParse((string?)node?.Attribute(name), NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? value : fallback;

    private static bool AttributeBool(XElement? node, string name, bool fallback)
        => node?.Attribute(name) is XAttribute value ? bool.TryParse(value.Value, out var result) ? result : fallback : fallback;

    private static string LegacyEdgeValue(EdgeValues edges) => edges.IsUniform
        ? edges.Top.ToString(CultureInfo.InvariantCulture)
        : edges.Top.ToString(CultureInfo.InvariantCulture);

    private static void SetEdgeFields(EdgeValues values, TextBox top, TextBox right, TextBox bottom, TextBox left)
    {
        top.Text = EdgeNumber(values.Top);
        right.Text = EdgeNumber(values.Right);
        bottom.Text = EdgeNumber(values.Bottom);
        left.Text = EdgeNumber(values.Left);
    }

    private static EdgeValues ReadEdgeFields(EdgeValues fallback, TextBox top, TextBox right, TextBox bottom, TextBox left) => new()
    {
        Top = EdgeNumber(top.Text, fallback.Top), Right = EdgeNumber(right.Text, fallback.Right),
        Bottom = EdgeNumber(bottom.Text, fallback.Bottom), Left = EdgeNumber(left.Text, fallback.Left)
    };

    private static string EdgeNumber(double value) => Math.Clamp(value, 0, 256).ToString("0.##", CultureInfo.InvariantCulture);

    private static double EdgeNumber(string text, double fallback = 0) =>
        double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
            ? Math.Clamp(value, 0, 256) : fallback;

    private static string OptionalNumber(double value) => value <= 0 ? "0" : value.ToString("0.##", CultureInfo.InvariantCulture);

    private static double OptionalNumber(string text, double fallback = 0)
    {
        if (string.IsNullOrWhiteSpace(text) || string.Equals(text.Trim(), "auto", StringComparison.OrdinalIgnoreCase)) return 0;
        return double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
            ? Math.Clamp(value, 0, 4096) : fallback;
    }

    private static double BoundedNumber(string text, double fallback, double minimum, double maximum)
    {
        return double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
            ? Math.Clamp(value, minimum, maximum) : fallback;
    }

    private static FontFamily SafeFont(string value)
    {
        try { return new FontFamily(value); }
        catch { return new FontFamily("Microsoft YaHei"); }
    }

    private static double Number(string text, double fallback) => double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? Math.Clamp(value, 0, 1) : fallback;

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e) { if (e.ClickCount == 2) Maximize_Click(sender, e); else if (e.ButtonState == MouseButtonState.Pressed) DragMove(); }
    private void Close_Click(object sender, RoutedEventArgs e) => Close();
    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Maximize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
}

internal sealed class CubicBezierEasing : EasingFunctionBase
{
    public CubicBezierEasing() { }
    public CubicBezierEasing(double p1x, double p1y, double p2x, double p2y) { P1X = p1x; P1Y = p1y; P2X = p2x; P2Y = p2y; }
    public double P1X { get; set; } = 0.25;
    public double P1Y { get; set; } = 0.1;
    public double P2X { get; set; } = 0.25;
    public double P2Y { get; set; } = 1;

    protected override double EaseInCore(double normalizedTime)
    {
        var t = Math.Clamp(normalizedTime, 0, 1);
        for (var i = 0; i < 8; i++)
        {
            var x = Cubic(t, P1X, P2X) - t;
            var derivative = 3 * (1 - t) * (1 - t) * P1X + 6 * (1 - t) * t * (P2X - P1X) + 3 * t * t * (1 - P2X);
            if (Math.Abs(derivative) < 1e-5) break;
            t = Math.Clamp(t - x / derivative, 0, 1);
        }
        return Cubic(t, P1Y, P2Y);
    }

    protected override Freezable CreateInstanceCore() => new CubicBezierEasing(P1X, P1Y, P2X, P2Y);
    private static double Cubic(double t, double p1, double p2) => 3 * (1 - t) * (1 - t) * t * p1 + 3 * (1 - t) * t * t * p2 + t * t * t;
}
