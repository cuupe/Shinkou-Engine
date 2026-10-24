"""Create small, generated assets for native editor QA; no downloaded media."""
import math
import pathlib
import struct
import subprocess
import sys
import wave
import zipfile

project = pathlib.Path(sys.argv[1]).resolve()
assets = project / "assets"
assets.mkdir(parents=True, exist_ok=True)
(assets / "preview.py").write_text("# Python source preview\n" + "\n".join(
    f'print("preview line {i}: 中文脚本")' for i in range(1, 81)), encoding="utf-8")
(assets / "notes.txt").write_text("UTF-16 text preview\n中文文档与脚本预览\n", encoding="utf-16")
xml = '''<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Shinkou 资源文档</w:t></w:r></w:p>
<w:p><w:r><w:t>DOCX 可以直接预览正文、标题和表格内容。</w:t></w:r></w:p>
<w:tbl><w:tr><w:tc><w:p><w:r><w:t>资源类型</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>预览能力</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:p><w:r><w:t>FBX / glTF</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>独立 3D 场景</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
</w:body></w:document>'''
with zipfile.ZipFile(assets / "document.docx", "w", zipfile.ZIP_DEFLATED) as doc:
    doc.writestr("[Content_Types].xml", '''<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>''')
    doc.writestr("word/document.xml", xml)
(assets / "triangle.fbx").write_text('''; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: { Version: 1000
 Properties70: { P: "UnitScaleFactor", "double", "Number", "",100 }
}
Objects: {
 Geometry: 1, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: -1,-0.7,0,1,-0.7,0,0,1,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
 }
 Model: 2, "Model::Triangle", "Mesh" { Version: 232 }
}
Connections: {
 C: "OO",1,2
 C: "OO",2,0
}
''', encoding="ascii")
with wave.open(str(assets / "tone.wav"), "wb") as audio:
    audio.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
    audio.writeframes(b"".join(struct.pack("<h", int(1800 * math.sin(2 * math.pi * 220 * i / 22050))) for i in range(22050 * 8)))
subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(assets / "tone.wav"), "-codec:a", "libmp3lame", "-b:a", "64k", str(assets / "tone.mp3")], check=True)
subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=3", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(assets / "preview.mp4")], check=True)
print(project)
