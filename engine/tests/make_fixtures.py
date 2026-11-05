#!/usr/bin/env python3
"""Generate the small PDF fixtures used by the engine tests.

Fixtures are produced by ReportLab (an independent PDF writer) so the engine is
tested against files it did not create. Run from repo root:
    python3 engine/tests/make_fixtures.py engine/tests/fixtures
"""
import io
import os
import sys

from reportlab.lib.pagesizes import letter, A4
from reportlab.pdfgen import canvas
from reportlab.lib.colors import black, blue
from PIL import Image

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "fixtures")
os.makedirs(out, exist_ok=True)


def text3():
    c = canvas.Canvas(os.path.join(out, "text3.pdf"), pagesize=letter, invariant=1)
    c.setTitle("Noodle Test Document")
    c.setAuthor("Agent Spaghetti")
    for i in range(1, 4):
        c.setFont("Helvetica-Bold", 20)
        c.drawString(72, 700, f"Chapter {i}")
        c.setFont("Helvetica", 12)
        c.drawString(72, 660, f"This is page {i} of the noodle test document.")
        if i == 2:
            c.drawString(72, 630, "The secret code is SECRET-CODE-1234 and the name is Jane Doe.")
            c.setFont("Times-Roman", 12)
            c.drawString(72, 600, "Jane Doe lives at 42 Linguine Lane.")
        c.setFont("Courier", 10)
        c.drawString(72, 100, f"footer marker {i}")
        c.showPage()
    c.save()


def text3_v2():
    """A second version of text3 with edits, for compare tests."""
    c = canvas.Canvas(os.path.join(out, "text3_v2.pdf"), pagesize=letter, invariant=1)
    for i in range(1, 4):
        c.setFont("Helvetica-Bold", 20)
        c.drawString(72, 700, f"Chapter {i}")
        c.setFont("Helvetica", 12)
        if i == 1:
            c.drawString(72, 660, f"This is page {i} of the spaghetti test document.")
        else:
            c.drawString(72, 660, f"This is page {i} of the noodle test document.")
        if i == 2:
            c.drawString(72, 630, "The secret code is SECRET-CODE-1234 and the name is Jane Doe.")
        if i == 3:
            c.drawString(72, 630, "A brand new sentence appears here.")
        c.setFont("Courier", 10)
        c.drawString(72, 100, f"footer marker {i}")
        c.showPage()
    c.save()


def form():
    c = canvas.Canvas(os.path.join(out, "form.pdf"), pagesize=letter, invariant=1)
    f = c.acroForm
    c.drawString(72, 720, "Name:")
    f.textfield(name="name", x=140, y=710, width=250, height=22, value="")
    c.drawString(72, 680, "Likes noodles:")
    f.checkbox(name="likes", x=180, y=676, size=16, checked=False, buttonStyle="check")
    c.drawString(72, 640, "Shape:")
    f.radio(name="shape", value="spiral", x=140, y=636, size=14, selected=False)
    f.radio(name="shape", value="tube", x=200, y=636, size=14, selected=False)
    c.drawString(72, 600, "Sauce:")
    f.choice(name="sauce", value="pesto", options=["pesto", "marinara", "alfredo"], x=140, y=594, width=150, height=20)
    c.showPage()
    c.save()


def image():
    img = Image.new("RGB", (200, 100), (200, 30, 30))
    for x in range(200):
        for y in range(40, 60):
            img.putpixel((x, y), (20, 20, 200))
    buf = io.BytesIO()
    img.save(buf, "PNG")
    buf.seek(0)
    from reportlab.lib.utils import ImageReader
    c = canvas.Canvas(os.path.join(out, "image.pdf"), pagesize=letter, invariant=1)
    c.drawImage(ImageReader(buf), 100, 400, width=400, height=200)
    c.setFont("Helvetica", 14)
    c.drawString(100, 380, "Caption under the picture")
    c.showPage()
    c.save()


def rotated():
    c = canvas.Canvas(os.path.join(out, "rotated.pdf"), pagesize=A4, invariant=1)
    c.setFont("Helvetica", 14)
    c.drawString(72, 750, "Rotated page text")
    c.showPage()
    c.save()
    # add /Rotate 90 with pypdf (independent writer)
    from pypdf import PdfReader, PdfWriter
    r = PdfReader(os.path.join(out, "rotated.pdf"))
    w = PdfWriter()
    for p in r.pages:
        p.rotate(90)
        w.add_page(p)
    with open(os.path.join(out, "rotated.pdf"), "wb") as fh:
        w.write(fh)


def ttf():
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
    pdfmetrics.registerFont(TTFont("DejaVu", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"))
    c = canvas.Canvas(os.path.join(out, "unicode.pdf"), pagesize=letter, invariant=1)
    c.setFont("DejaVu", 14)
    c.drawString(72, 700, "Crème brûlée — naïve café ñoquis")
    c.drawString(72, 670, "Private number 555-0199")
    c.showPage()
    c.save()


def broken():
    with open(os.path.join(out, "text3.pdf"), "rb") as fh:
        data = fh.read()
    i = data.rfind(b"startxref")
    data = data[:i] + b"startxref\n999999\n%%EOF\n"
    with open(os.path.join(out, "broken_xref.pdf"), "wb") as fh:
        fh.write(data)


def many(n=300):
    c = canvas.Canvas(os.path.join(out, "many.pdf"), pagesize=letter, invariant=1)
    for i in range(1, n + 1):
        c.setFont("Helvetica", 24)
        c.drawString(72, 700, f"Page number {i}")
        c.showPage()
    c.save()


def adversarial():
    """Hand-built hostile PDFs found during the security review (nested forms, odd /Rotate,
    oversized CropBox, secrets outside page content)."""
    def build(objs, root=1, info=None):
        out = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"; offs=[]
        for i,o in enumerate(objs,1):
            offs.append(len(out))
            if isinstance(o, tuple):
                d, s = o
                out += f"{i} 0 obj\n{d[:-2]} /Length {len(s)} >>\nstream\n".encode()+s+b"\nendstream\nendobj\n"
            else:
                out += f"{i} 0 obj\n{o}\nendobj\n".encode()
        x = len(out)
        out += f"xref\n0 {len(objs)+1}\n0000000000 65535 f \n".encode()
        for o in offs: out += f"{o:010d} 00000 n \n".encode()
        tr = f"trailer\n<< /Size {len(objs)+1} /Root {root} 0 R" + (f" /Info {info} 0 R" if info else "") + " >>\n"
        out += tr.encode() + f"startxref\n{x}\n%%EOF\n".encode()
        return out
    FONT = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
    
    # A: nested form xobjects 14 deep
    N=14
    objs = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> /XObject << /X0 6 0 R >> >> /Contents 4 0 R >>",
            ("<< >>", b"BT /F1 24 Tf 72 720 Td (Public header) Tj ET\n/X0 Do"), FONT]
    for k in range(N):
        nxt = 6+k+1
        if k < N-1:
            objs.append((f"<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> /XObject << /X0 {nxt} 0 R >> >> >>", b"/X0 Do"))
        else:
            objs.append(("<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> >>", b"BT /F1 24 Tf 72 500 Td (TOPSECRET NESTED) Tj ET"))
    open(os.path.join(out, "nested.pdf"),"wb").write(build(objs))
    
    # B: /Rotate 135 (engine treats as 90, pdf.js as 0)
    objs = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate 135 /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
            ("<< >>", b"BT /F1 24 Tf 72 720 Td (Public header) Tj ET BT /F1 24 Tf 72 500 Td (ROTSECRET 12345) Tj ET"), FONT]
    open(os.path.join(out, "rot135.pdf"),"wb").write(build(objs))
    
    # B2: CropBox larger than MediaBox
    objs = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /CropBox [0 0 612 1584] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
            ("<< >>", b"BT /F1 24 Tf 72 720 Td (Public header) Tj ET BT /F1 24 Tf 72 500 Td (CROPSECRET 999) Tj ET"), FONT]
    open(os.path.join(out, "crop.pdf"),"wb").write(build(objs))
    
    # C: term on page + elsewhere (form widget, sticky note, outline, info)
    ap = b"/Tx BMC q BT /F1 14 Tf 2 6 Td (John Smith) Tj ET Q EMC"
    objs = ["<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R] /DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv 5 0 R >> >> >> /Outlines 8 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /Annots [6 0 R 10 0 R] >>",
            ("<< >>", b"BT /F1 24 Tf 72 720 Td (Client: John Smith) Tj ET BT /F1 12 Tf 72 420 Td (Emergency contact:) Tj ET"), FONT,
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (contact) /V (John Smith) /Rect [200 410 400 435] /P 3 0 R /F 4 /DA (/Helv 14 Tf 0 g) /AP << /N 7 0 R >> >>",
            ("<< /Type /XObject /Subtype /Form /BBox [0 0 200 25] /Resources << /Font << /F1 5 0 R >> >> >>", ap),
            "<< /Type /Outlines /First 9 0 R /Last 9 0 R /Count 1 >>",
            "<< /Title (John Smith file) /Parent 8 0 R /Dest [3 0 R /XYZ 0 792 0] >>",
            "<< /Type /Annot /Subtype /Text /Rect [500 100 520 120] /Contents (Note about John Smith) /F 4 /Open false >>",
            "<< /Title (John Smith) /Author (John Smith) >>"]
    open(os.path.join(out, "elsewhere.pdf"),"wb").write(build(objs, info=11))


text3()
text3_v2()
adversarial()
form()
image()
rotated()
ttf()
broken()
many()
print("fixtures written to", out)
