# XML 高度定制样式示例

样式属性采用字符串值，结构化属性走快速路径，扩展属性保留在 rule 中。这样既能保持运行时性能，又不会把样式能力锁死。

~~~xml
<ui-styles schema="shinkou-ui" version="1">
  <theme id="studio-light" scale="1" reduce-motion="false">
    <font family="Microsoft YaHei" size="14" weight="400" />
    <palette>
      <color name="accent" value="#4C76F5" />
      <color name="surface" value="#FFFFFF" />
      <color name="text" value="#22232A" />
    </palette>
    <controls>
      <control type="button" background="surface" text="text"
               radius="8" border-width="1" transition="120" />
    </controls>
    <rules>
      <rule selector=".button" state="hover">
        <property name="background" value="#5B82FF" />
        <property name="box-shadow" value="0 8px 24px #00000022" />
        <property name="transform" value="translateY(-1px)" />
      </rule>
      <rule selector="#danger-button" state="pressed">
        <property name="background" value="#D84A4A" />
        <property name="radius-top-left" value="12" />
        <property name="radius-bottom-right" value="2" />
      </rule>
    </rules>
  </theme>
</ui-styles>
~~~

StyleSheet::property(selector, state, name) 提供扩展属性查询；控件绘制时会优先读取 rule，再回退到结构化 ControlStyle，因此常用属性不需要付出动态字典查找的全部成本。
