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


text3()
text3_v2()
form()
image()
rotated()
ttf()
broken()
many()
print("fixtures written to", out)
