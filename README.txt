GPS Assist Data Updater 1.0.0
=============================

Updates the GPS Assist Data on the memory card of Sony cameras with built-in GPS
(Alpha SLT-A55V / A65V / A77V / A99V, and other Sony "V" models with GPS).

Which file to run
  GpsAssistUpdater-x64.exe    Windows 10/11 64-bit (Intel/AMD)
  GpsAssistUpdater-x86.exe    32-bit Windows; also runs on any 64-bit Windows
  GpsAssistUpdater-arm64.exe  Windows 11 on ARM (Snapdragon)
No installation is needed. It does not need administrator rights.

How to use
  1. Put the camera's memory card in the PC (or connect the camera by USB in
     Mass Storage mode).
  2. Choose the card in "Memory card" and click "Update GPS Assist Data".
  3. Safely remove the card, put it back in the camera and turn the camera on.
     The camera loads the new data by itself.
  The data is valid for about 30 days, so update again after that.

What it does
  Downloads assistme.dat from Sony's server (control.d-imaging.sony.co.jp),
  checks it against Sony's MD5, writes it to \PRIVATE\SONY\GPS\assistme.dat on
  the card, and reads it back from the card to confirm the write.
  The last download is kept in %LOCALAPPDATA%\GpsAssistUpdater, so more cards can
  be updated (even offline) while that copy is under 30 days old.

Not made by or affiliated with Sony.

-----------------------------------------------------------------------------

โปรแกรมอัปเดตข้อมูล GPS Assist 1.0.0

ใช้อัปเดตข้อมูล GPS Assist ในการ์ดหน่วยความจำ สำหรับกล้อง Sony ที่มี GPS ในตัว
(Alpha SLT-A55V / A65V / A77V / A99V และกล้อง Sony รุ่น "V" อื่น ๆ ที่มี GPS)

เลือกไฟล์ที่จะใช้
  GpsAssistUpdater-x64.exe    Windows 10/11 แบบ 64 บิต (Intel/AMD)
  GpsAssistUpdater-x86.exe    Windows แบบ 32 บิต (ใช้กับ Windows 64 บิตได้ด้วย)
  GpsAssistUpdater-arm64.exe  Windows 11 บน ARM (Snapdragon)
ไม่ต้องติดตั้ง และไม่ต้องใช้สิทธิ์ผู้ดูแลระบบ

วิธีใช้
  1. เสียบการ์ดหน่วยความจำของกล้องเข้ากับคอมพิวเตอร์ (หรือต่อกล้องด้วยสาย USB
     ในโหมด Mass Storage)
  2. เลือกการ์ดที่ช่อง "การ์ดหน่วยความจำ" แล้วกด "อัปเดตข้อมูล GPS Assist"
  3. ถอดการ์ดอย่างปลอดภัย ใส่กลับเข้ากล้องแล้วเปิดกล้อง กล้องจะโหลดข้อมูลใหม่เอง
  ข้อมูลมีอายุประมาณ 30 วัน หลังจากนั้นให้อัปเดตใหม่

โปรแกรมนี้ไม่ได้จัดทำโดย Sony และไม่มีส่วนเกี่ยวข้องกับ Sony
