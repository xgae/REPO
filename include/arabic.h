// Arabic Strings for v4.2 - UTF-8 + RTL ready
#ifndef ARABIC_H
#define ARABIC_H

#ifdef LANG_ARABIC
  #define MSG_SYS        "نظام الحضور"
  #define MSG_READY      "النظام جاهز ✓"
  #define MSG_SCAN       "ضع إصبعك"
  #define MSG_WIFI_OK    "واي فاي OK"
  #define MSG_SENSOR_OK  "الحساس OK"
  #define MSG_ENROLL_OK  "تم التسجيل ✓"
  #define MSG_UNKNOWN    "غير معروف ✗"
  #define MSG_MATCHED    "تطابق ✓"
  #define MSG_WEAK       "مسح ضعيف"
  #define MSG_ASK_ADMIN  "اطلب المشرف"
  #define MSG_ORPHAN     "مسح يتيم"
  #define MSG_DATA_CLEAR "تم مسح البيانات"
#else
  #define MSG_SYS        "* Attendance Sys *"
  #define MSG_READY      " System Ready! ✓"
  #define MSG_SCAN       "Place finger to scan"
  #define MSG_WIFI_OK    "WiFi OK!"
  #define MSG_SENSOR_OK  "Sensor OK ✓"
  #define MSG_ENROLL_OK  " Enroll OK! ✓ "
  #define MSG_UNKNOWN    "!! NOT FOUND !! ✗"
  #define MSG_MATCHED    " Matched! ✓ "
  #define MSG_WEAK       "!! WEAK SCAN !!"
  #define MSG_ASK_ADMIN  "Ask admin to enroll"
  #define MSG_ORPHAN     "!! ORPHAN SCAN !!"
  #define MSG_DATA_CLEAR "Data Cleared!"
#endif

// Schedule Arabic
#ifdef LANG_ARABIC
  #define LESSON_0_EN "Comp Maint S1"
  #define LESSON_0_AR "صيانة الحاسب 1"
  #define LESSON_1_EN "Comp Maint S2"
  #define LESSON_1_AR "صيانة الحاسب 2"
  #define LESSON_2_EN "Comp Maint S3"
  #define LESSON_2_AR "صيانة الحاسب 3"
  #define LESSON_3_EN "Circuit Design"
  #define LESSON_3_AR "تصميم الدوائر"
  #define LESSON_4_EN "Networking"
  #define LESSON_4_AR "الشبكات"
#else
  #define LESSON_0_EN "Comp Maint S1"
  #define LESSON_0_AR LESSON_0_EN
  #define LESSON_1_EN "Comp Maint S2"
  #define LESSON_1_AR LESSON_1_EN
  #define LESSON_2_EN "Comp Maint S3"
  #define LESSON_2_AR LESSON_2_EN
  #define LESSON_3_EN "Circuit Design"
  #define LESSON_3_AR LESSON_3_EN
  #define LESSON_4_EN "Networking"
  #define LESSON_4_AR LESSON_4_EN
#endif

#endif // ARABIC_H

