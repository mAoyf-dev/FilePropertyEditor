"""Optional fixture regeneration. Neither dependency is used by the application."""
from pathlib import Path
from datetime import datetime, timezone
from PIL import Image
from docx import Document

folder = Path(__file__).parent / "fixtures"
folder.mkdir(exist_ok=True)
image = Image.new("RGB", (16, 16), (30, 120, 150))
exif = Image.Exif()
exif[271] = "Test Camera"
exif[272] = "FPV Test"
exif[306] = "2018:04:05 06:07:08"
exif[34665] = {36867: "2018:04:05 06:07:08", 36868: "2018:04:05 06:07:08"}
image.save(folder / "exif.jpg", exif=exif)
document = Document()
document.add_paragraph("TEST DOCUMENT CONTENT")
document.core_properties.title = "Before"
document.core_properties.created = datetime(2018, 4, 5, 6, 7, 8, tzinfo=timezone.utc)
document.save(folder / "document.docx")
