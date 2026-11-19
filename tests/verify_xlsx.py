"""Optional independent export check; uses the existing local openpyxl installation."""
from openpyxl import load_workbook
from docx import Document

sheet = load_workbook("build/verification.xlsx").active
assert sheet["A2"].value == "=1+1"
assert sheet["A2"].data_type == "s"
assert sheet["B2"].value == '中文,"引号"\n换行'
assert sheet.max_row == 2 and sheet.max_column == 2
print("Independent XLSX readback passed")
document = Document("build/verification.docx")
assert document.core_properties.title == "修改后的中文标题"
assert document.paragraphs[0].text == "TEST DOCUMENT CONTENT"
assert document.core_properties.created is None
print("Independent DOCX readback passed")
