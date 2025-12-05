#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>
#include <WiFiEsp.h>
#include <WiFiEspClient.h>

// ===================== Pins & Hardware =====================
#define RST_PIN   9               // RFID RST
#define SS_PIN   10               // RFID SS

// 키패드: R1 + C1/C2/C3 세 키만 사용 (모드 선택)
#define KP_ROW_R1      8          // R1 (OUTPUT, LOW로 구동)
#define KP_COL_BORROW  2          // C1 (INPUT_PULLUP) → 대출
#define KP_COL_RETURN  3          // C2 (INPUT_PULLUP) → 반납
#define KP_COL_SCANEND 4             // C3 (INPUT_PULLUP) → 스캔 완료 S3
// ESP-01 (SoftwareSerial)
#define WIFIRX 6                  // Arduino RX  ← ESP TX
#define WIFITX 7                  // Arduino TX  → ESP RX

// ===== RGB LED =====
// 핀은 필요에 맞게 변경하세요.
#define LED_R  A0
#define LED_G  A1
#define LED_B  A2
// 공통애노드면 1, 공통캐소드면 0
#define COMMON_ANODE 0

// ===================== Objects =====================
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
SoftwareSerial wifiSerial(WIFIRX, WIFITX);
WiFiEspClient client;

// --- 최근 RFID UID 보관 (SCANEND 전송 시 사용) ---
String currentUid = "";      

// ===================== WiFi / Server =====================
#define AP_SSID "iotA"
#define AP_PASS "iotA1234"
#define SERVER_NAME "10.10.14.90"
#define SERVER_PORT 5000

#define LOGID  "LIB_ARD"
#define PASSWD "PASSWD"

// ===================== State =====================
enum Mode  { MODE_NONE = 0, MODE_BORROW, MODE_RETURN };
enum State { ST_MENU = 0, ST_WAIT_RFID, ST_WAIT_SCANEND, ST_WAIT_RESULT };

Mode  curMode  = MODE_NONE;
State curState = ST_MENU;

unsigned long lastDebounceMs = 0;
const unsigned long DEBOUNCE_MS = 40;

// --- RETURN 누적/2단계 표시용 ---
bool pendingReturnHasDeny = false;   // 스캔 중 DENY가 하나라도 왔는지
int  pendingOverdueDays   = 0;       // 스캔 중 최대 연체일수

bool returnTwoStage = false;         // S3 이후 2단계 표시 사용 여부
int  returnStage    = 0;             // 0: 1단계(연체), 1: 2단계(성공)
unsigned long returnStageTs = 0;     // 스테이지 전환 타이머

// 결과 표시 제어

bool     resultReceived = false;
bool     resultOK       = false;   // true=대출 가능, false=대출 불가능
String   resultText     = "";      // "권수 초과" / "연체" 등
unsigned long resultTs  = 0;
// === 결과 표시 유지 시간(ms) ===
unsigned long resultHoldMs = 5000;   // 기본 2초, "반납 성공"은 5초로 변경

// ===================== Forward Decls =====================
void showMenu();
void showAuth();
void wifi_Init();
void server_Connect();
void sendUIDMODEtoServer(const String& uid);
bool btnPressedBorrow();
bool btnPressedReturn();
String readUID();
void processIncomingLine(char *line);
int extractFirstNumber(const String& src);
// ===== RGB helpers =====
inline void rgbWriteRaw(bool r, bool g, bool b) {
  // 공통애노드/캐소드에 맞게 반전
  if (COMMON_ANODE) { r = !r; g = !g; b = !b; }
  digitalWrite(LED_R, r ? HIGH : LOW);
  digitalWrite(LED_G, g ? HIGH : LOW);
  digitalWrite(LED_B, b ? HIGH : LOW);
}
void rgbOff()        { rgbWriteRaw(false, false, false); }
void rgbRed()        { rgbWriteRaw(true , false, false); }
void rgbGreen()      { rgbWriteRaw(false, true , false); }
void rgbBlue()       { rgbWriteRaw(false, false, true ); } // 처리중 표시용

bool btnPressedScanEnd() {
  static int last = HIGH;
  int r = digitalRead(KP_COL_SCANEND); // LOW=pressed
  if (millis() - lastDebounceMs > DEBOUNCE_MS) {
    if (last == HIGH && r == LOW) {
      last = r; lastDebounceMs = millis();
      Serial.println("[KEYPAD] SCANEND pressed");
      while (digitalRead(KP_COL_SCANEND) == LOW) {}
      delay(10);
      Serial.println("[KEYPAD] SCANEND released");
      return true;
    }
    last = r;
  }
  return false;
}
void sendScanEndToServer(const String& uid) {
  if (!client.connected()) server_Connect();
  // 현재 UID가 바로 앞 단계에서 보낸 사용자 UID와 동일하다고 가정
  // 세션ID가 있다면 여기서 함께 붙이면 더 안전: SCANEND@UID@SID@
  String msg = "[LIB_SQL]SCANEND@" + uid + "@\n";
  client.print(msg);
  client.flush();
  Serial.print("[TX] "); Serial.print(msg);
}

// "2025-08-26" 같은 YYYY-MM-DD 첫 패턴 추출
String extractDateYYYYMMDD(const String& s) {
  for (int i = 0; i + 9 < (int)s.length(); ++i) {
    if (isDigit(s[i]) && isDigit(s[i+1]) && isDigit(s[i+2]) && isDigit(s[i+3]) &&
        s[i+4]=='-' && isDigit(s[i+5]) && isDigit(s[i+6]) &&
        s[i+7]=='-' && isDigit(s[i+8]) && isDigit(s[i+9])) {
      return s.substring(i, i+10);
    }
  }
  return "";
}
void setup() {
  // 키패드
  pinMode(KP_ROW_R1, OUTPUT);
  digitalWrite(KP_ROW_R1, LOW);          // 행 LOW → 눌리면 열이 LOW
  pinMode(KP_COL_BORROW, INPUT_PULLUP);
  pinMode(KP_COL_RETURN, INPUT_PULLUP);
  pinMode(KP_COL_SCANEND, INPUT_PULLUP);
  // RGB
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  rgbOff();

  // 주변장치
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();

  lcd.begin(16, 2);
  lcd.backlight();
  showMenu();

  // 네트워크
  wifiSerial.begin(38400);
  wifi_Init();
  server_Connect();
}

// ===================== LOOP =====================
void loop() {
  // 주기적 연결 유지
  static unsigned long lastChk = 0;
  if (millis() - lastChk > 5000) {
    lastChk = millis();
    if (!client.connected()) server_Connect();
  }

  // 서버 수신 처리 (한 줄 단위)
  if (client.available()) {
    char buf[160] = {0};
    size_t n = client.readBytesUntil('\n', buf, sizeof(buf)-1);
    if (n > 0) {
      Serial.print("[RX] "); Serial.println(buf);
      processIncomingLine(buf);
    }
  }

   // ----- 상태 머신 -----
  switch (curState) {
    case ST_MENU: {
      if (btnPressedBorrow()) {
        curMode = MODE_BORROW;
        rgbOff();
        showAuth();
        curState = ST_WAIT_RFID;
      } else if (btnPressedReturn()) {
        curMode = MODE_RETURN;
        rgbOff();
        showAuth();
        curState = ST_WAIT_RFID;
      }
    } break;

    case ST_WAIT_RFID: {
      String uid = readUID();
      if (uid.length() > 0) {
        sendUIDMODEtoServer(uid);

        lcd.clear();
        lcd.setCursor(0,0); lcd.print("Scan books...");
        lcd.setCursor(0,1); lcd.print("Press S3");
        rgbBlue();

        currentUid = uid;
        curState   = ST_WAIT_SCANEND;
      }
    } break;

    case ST_WAIT_SCANEND: {
      // 대출모드: 스캔 중 DENY 오면 즉시 결과로
      if (resultReceived && curMode == MODE_BORROW) {
        if (!resultOK) {                // DENY
          curState = ST_WAIT_RESULT;
          break;
        } else {
          resultReceived = false;       // OK면 계속 스캔
        }
      }

      // S3: 스캔 종료
      if (btnPressedScanEnd()) {
        sendScanEndToServer(currentUid);

        lcd.clear();
        lcd.setCursor(0,0); lcd.print("Processing...");
        lcd.setCursor(0,1); lcd.print("Please wait");
        rgbBlue();

        if (curMode == MODE_RETURN) {
          if (pendingOverdueDays > 0) {
            // 두 단계 표시: 1) 연체 n일 → 2) Success
            returnTwoStage = true;
            returnStage    = 0;
            resultOK       = true; // 반납 자체는 OK
            resultText     = "Overdue " + String(pendingOverdueDays) + "day";
            resultReceived = true; // 바로 ST_WAIT_RESULT에서 1단계 출력
          } else if (pendingReturnHasDeny) {
            returnTwoStage = false;
            resultOK       = false;
            resultText     = "Return denied";
            resultReceived = true;
          } else {
            returnTwoStage = false;
            resultOK       = true;
            resultText     = "";
            resultReceived = true;
          }
          pendingReturnHasDeny = false;
          pendingOverdueDays   = 0;
        } else {
          // 대출은 서버 최종 응답 대기
          resultReceived = false;
          resultText     = "";
        }

        curState = ST_WAIT_RESULT;
      }
    } break;

    case ST_WAIT_RESULT: {
      if (resultReceived) {
        // ---- 반납 2단계 표시 모드 ----
        if (curMode == MODE_RETURN && returnTwoStage) {
          // 1단계: 연체 n일 (3초)
          if (returnStage == 0) {
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("Return Info");
            lcd.setCursor(0,1); lcd.print(resultText);
            rgbRed();
            returnStageTs = millis();
            returnStage   = 1;      // 다음은 성공 단계
            resultReceived = false; // 1단계 표시 중
          }

          // 2단계로 전환(3초 경과)
          if (returnStage == 1 && millis() - returnStageTs > 4000) {
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("Return Success!");
            lcd.setCursor(0,1); lcd.print("");
            rgbGreen();
            returnStageTs = millis();
            returnStage   = 2;      // 완료 대기
          }

          // 2단계 종료(5초 경과)
          if (returnStage == 2 && millis() - returnStageTs > 5000) {
            returnTwoStage = false;
            returnStage    = 0;
            resultTs       = 0;
            curMode        = MODE_NONE;
            showMenu();
            rgbOff();
            curState       = ST_MENU;
          }

          // 2단계 모드일 땐 공통 타임아웃 사용 안 함
          break;
        }

        // ---- 단일 단계 처리(대출/반납 공통) ----
        if (curMode == MODE_RETURN) {
          if (resultOK) {
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("Return Success!");
            lcd.setCursor(0,1); lcd.print("");
            rgbGreen();
            resultHoldMs = 5000;
          } else {
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("Return Fail");
            lcd.setCursor(0,1); lcd.print(resultText);
            rgbRed();
            resultHoldMs = 5000;
          }
        } else { // MODE_BORROW
          if (resultOK) {
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("CheckOut Success!");
            lcd.setCursor(0,1); lcd.print("");
            rgbGreen();
            resultHoldMs = 5000;
          } else {
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("CheckOut Fail");
            lcd.setCursor(0,1); lcd.print(resultText);
            rgbRed();
            resultHoldMs = 5000;
          }
        }
        resultTs = millis();
        resultReceived = false;
      }

      // 단일 단계 공통 타임아웃
      if (!returnTwoStage && resultTs && millis() - resultTs > resultHoldMs) {
        resultTs = 0;
        curMode = MODE_NONE;
        showMenu();
        rgbOff();
        curState = ST_MENU;
      }
    } break;
  } // <-- end switch

} // <-- end loop



// ===================== LCD helpers =====================
void showMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Select Mode");
  lcd.setCursor(0,1); lcd.print("Borrow:1Return:2");
}

void showAuth() {
  lcd.clear();
  lcd.setCursor(0,0);
  if (curMode == MODE_BORROW) lcd.print("Mode: Borrow");
  else if (curMode == MODE_RETURN) lcd.print("Mode: Return");
  lcd.setCursor(0,1);
  lcd.print("RFID TAG");
}

// ===================== WiFi / Server =====================
void wifi_Init() {
  WiFi.init(&wifiSerial);
  while (WiFi.begin(AP_SSID, AP_PASS) != WL_CONNECTED) {
    Serial.println("WiFi retry...");
    delay(2000);
  }
  Serial.println("WiFi Connected");
}

void server_Connect() {
  if (client.connect(SERVER_NAME, SERVER_PORT)) {
    Serial.println("Connected to server");
    client.print("[" LOGID ":" PASSWD "]");  // 인증
    client.flush();
  } else {
    Serial.println("Server connection failed");
  }
}

// 모드 + UID 전송
void sendUIDMODEtoServer(const String& uid) {
  if (!client.connected()) server_Connect();
  const char* modeStr = (curMode == MODE_BORROW) ? "CHECKOUT" : "RETURN";
  String msg = "[LIB_SQL]" + String(modeStr) + "@" + uid + "@" + "\n";
  client.print(msg);
  client.flush();
  Serial.print("[TX] "); Serial.print(msg);
}

// ===================== Keypad / RFID =====================
bool btnPressedBorrow() {
  static int last = HIGH;
  int r = digitalRead(KP_COL_BORROW); // LOW=pressed
  if (millis() - lastDebounceMs > DEBOUNCE_MS) {
    if (last == HIGH && r == LOW) {
      last = r; lastDebounceMs = millis();
      Serial.println("[KEYPAD] BORROW pressed");
      while (digitalRead(KP_COL_BORROW) == LOW) {}
      delay(10);
      Serial.println("[KEYPAD] BORROW released");
      return true;
    }
    last = r;
  }
  return false;
}
bool btnPressedReturn() {
  static int last = HIGH;
  int r = digitalRead(KP_COL_RETURN); // LOW=pressed
  if (millis() - lastDebounceMs > DEBOUNCE_MS) {
    if (last == HIGH && r == LOW) {
      last = r; lastDebounceMs = millis();
      Serial.println("[KEYPAD] RETURN pressed");
      while (digitalRead(KP_COL_RETURN) == LOW) {}
      delay(10);
      Serial.println("[KEYPAD] RETURN released");
      return true;
    }
    last = r;
  }
  return false;
}

String readUID() {
  if (!rfid.PICC_IsNewCardPresent()) return "";
  if (!rfid.PICC_ReadCardSerial())   return "";

  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidStr += "0";
    uidStr += String(rfid.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase();
  rfid.PICC_HaltA();
  return uidStr;
}
// 문자열에서 첫 번째 숫자(연속된 숫자들)를 찾아 정수로 반환 (없으면 0)
int extractFirstNumber(const String& src) {
  int val = 0, found = 0;
  for (size_t i = 0; i < src.length(); ++i) {
    if (isDigit(src[i])) { val = val * 10 + (src[i]-'0'); found = 1; }
    else if (found) break;
  }
  return found ? val : 0;
}

// ===================== Server Response Parser =====================
// 통일 포맷 가정:
//   [LIB_SQL]CHECKOUT@OK
//   [LIB_SQL]CHECKOUT@DENY@LIMIT
//   [LIB_SQL]CHECKOUT@DENY@OVERDUE
// ====== 통일 포맷 파서: CHECKOUT/RETURN OK or DENY (+사유/일수) ======
void processIncomingLine(char *line) {
  String s = String(line);
  String slow = s; slow.toLowerCase();

  // 기본값
  resultOK   = false;
  resultText = "";

  // ---- CHECKOUT (대출) ----
  if (s.indexOf("CHECKOUT@OK") >= 0) {
    resultOK = true;
  }
  else if (s.indexOf("CHECKOUT@DENY@LIMIT") >= 0) {
    resultOK = false;
    resultText = "Books exceeded";
  }
  else if (s.indexOf("CHECKOUT@DENY@OVERDUE") >= 0) {
  resultOK = false;
  // 1) 날짜 형식 우선 처리
  String date = extractDateYYYYMMDD(s);
  if (date.length() > 0) {
    // 원하는 문구로: "Overdue until 2025-08-29" 또는 "Banned until ..."
    resultText = "Overdue until " + date;   // <- 여기 문구는 취향대로
  } else {
    // 2) 날짜가 없으면 일수 파싱으로 fallback
    int days = extractFirstNumber(s);
    resultText = (days > 0) ? ("Overdue " + String(days) + "day")
                            : "Overdue";
  }
}
  // ★ 대출 금지 날짜 메시지 처리: [LIB_SQL]FAIL@Banned until 2025-08-26
  else if (slow.indexOf("fail@banned until") >= 0 || slow.indexOf("banned until") >= 0) {
    resultOK = false;
    String date = extractDateYYYYMMDD(s);
    resultText = (date.length() ? (date) : "Banned user");
  }

  // ---- RETURN (반납) ----
  else if (s.indexOf("RETURN@OK") >= 0) {
    resultOK = true;
  }
  else if (s.indexOf("RETURN@DENY@OVERDUE") >= 0) {
    resultOK = false;
    int days = extractFirstNumber(s);
    if (days > 0) {
      resultText = "Overdue " + String(days) + "day";
      if (days > pendingOverdueDays) pendingOverdueDays = days; // 최대치 갱신
    } else {
      resultText = "Overdue";
    }
  }
  // 필요하면 NOT_BORROWED 등 추가

  // ===== 핵심: 반납모드 + 스캔중이면 결과 '보류', 그 외는 즉시 반영 =====
  if (curMode == MODE_RETURN && curState == ST_WAIT_SCANEND) {
    if (!resultOK) pendingReturnHasDeny = true;  // DENY만 기록
    resultReceived = false;                      // 즉시 LCD 갱신 안함
  } else {
    resultReceived = true;                       // 대출/결과단계는 즉시 반영
  }
}


