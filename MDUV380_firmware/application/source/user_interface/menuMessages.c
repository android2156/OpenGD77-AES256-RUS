/*
 * menuMessages.c — on-radio encrypted DMR SMS UX (Inbox / Sent / New Message)
 * with Russian/English UI localisation and Russian/English multi-tap input (# toggle).
 */
#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "functions/trx.h"
#include "functions/dmr_sms.h"
#include "crypto/dmr_aes.h"
#include "io/keyboard.h"
#include <string.h>
#include <stdlib.h>

#if defined(ENABLE_DMR_DATA)

extern volatile bool keypadAlphaRussian;
extern void keyboardResetAlpha(void);
#if ! defined(LANGUAGE_BUILD_JAPANESE)
extern const stringsTable_t russianLanguage;
#endif

static inline bool isRussianUI(void)
{
#if ! defined(LANGUAGE_BUILD_JAPANESE)
	return (currentLanguage == &russianLanguage);
#else
	return false;
#endif
}

#define TR(en, ru) (isRussianUI() ? (ru) : (en))

enum { MSG_HOME = 0, MSG_LIST, MSG_READ, MSG_COMPOSE, MSG_RECIPIENT, MSG_RESULT };

static struct
{
	uint8_t  view;
	uint8_t  folder;        // 0 = Inbox, 1 = Sent
	int16_t  readIdx;       // message being read
	char     compose[DMR_SMS_TEXT_MAX + 1];
	int16_t  composePos;
	char     rcpt[10];      // numeric recipient entry (up to 8 digits for 24-bit DMR ID)
	int16_t  rcptPos;
	uint8_t  rcptGroup;     // 1 = talkgroup, 0 = private call
	int8_t   presetIdx;     // last quick-text preset cycled in (-1 = none)
	int8_t   result;        // dmrSmsSend() return for the result screen
	uint16_t resultTicks;
} s_msg DMR_AES_CCM;

static void homeUpdate(void);
static void listUpdate(void);
static void readUpdate(void);
static void composeUpdate(void);
static void recipientUpdate(void);
static void resultUpdate(void);
static void homeEvent(uiEvent_t *ev, menuStatus_t *ec);
static void listEvent(uiEvent_t *ev);
static void readEvent(uiEvent_t *ev);
static void composeEvent(uiEvent_t *ev);
static void recipientEvent(uiEvent_t *ev);

static void gotoHome(void)
{
	keypadAlphaEnable = false;
	keypadAlphaRussian = false;
	keyboardResetAlpha();
	s_msg.view = MSG_HOME;
	menuDataGlobal.currentItemIndex = 0;
	menuDataGlobal.numItems = 3;
	homeUpdate();
}

menuStatus_t menuMessages(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		memset(&s_msg, 0, sizeof s_msg);
		dmrSmsInit();
		gotoHome();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (s_msg.view == MSG_RESULT)
	{
		if (s_msg.resultTicks) { s_msg.resultTicks--; }
		if ((s_msg.resultTicks == 0) || (ev->hasEvent && (ev->events & KEY_EVENT)))
		{
			gotoHome();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}
		return exitCode;
	}

	if (ev->hasEvent)
	{
		switch (s_msg.view)
		{
			case MSG_HOME:      homeEvent(ev, &exitCode); break;
			case MSG_LIST:      listEvent(ev);            break;
			case MSG_READ:      readEvent(ev);            break;
			case MSG_COMPOSE:   composeEvent(ev);         break;
			case MSG_RECIPIENT: recipientEvent(ev);       break;
		}
	}
	return exitCode;
}

/* ============================ HOME ===================================== */
static void homeUpdate(void)
{
	char buf[24];
	displayClearBuf();
	menuDisplayTitle((char *)currentLanguage->messages);

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(3, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		switch (mNum)
		{
			case 0:  snprintf(buf, sizeof buf, TR("Inbox (%d)", "\302\365\356\344\377\371\350\345 (%d)"), dmrSmsCount(0)); break;
			case 1:  snprintf(buf, sizeof buf, TR("Sent (%d)",  "\316\362\357\360\340\342\353. (%d)"), dmrSmsCount(1)); break;
			default: snprintf(buf, sizeof buf, "%s", TR("New Message", "\315\356\342\356\345 \361\356\356\341\371."));           break;
		}
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}

	{
		uint32_t d[7];
		char l1[26], l2[26];
		dmrSmsRxDiag(d);
		snprintf(l1, sizeof l1, "d%lu h%lu!%lu b%lu!%lu",
				(unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2],
				(unsigned long)d[3], (unsigned long)d[4]);
		snprintf(l2, sizeof l2, "p%lu m%lu", (unsigned long)d[5], (unsigned long)d[6]);
		displayPrintCentered(108, l1, FONT_SIZE_1);
		displayPrintCentered(118, l2, FONT_SIZE_1);
	}

	displayRender();
}

static void openFolderAt(int folder, int cursor)
{
	s_msg.folder = (uint8_t)folder;
	s_msg.view = MSG_LIST;
	menuDataGlobal.numItems = dmrSmsCount(folder) + 1;   // +1 for "[Delete all]"
	if (cursor >= menuDataGlobal.numItems) { cursor = menuDataGlobal.numItems - 1; }
	if (cursor < 0) { cursor = 0; }
	menuDataGlobal.currentItemIndex = cursor;
	listUpdate();
}

static void openFolder(int folder)
{
	openFolderAt(folder, 0);
}

static void startCompose(void)
{
	memset(s_msg.compose, 0, sizeof s_msg.compose);
	s_msg.composePos = 0;
	s_msg.presetIdx = -1;
	s_msg.view = MSG_COMPOSE;
	keypadAlphaRussian = true;   // Default to Russian [РУС] when opening New Message
	keyboardResetAlpha();
	keypadAlphaEnable = true;
	composeUpdate();
}

static void cyclePreset(void)
{
	int total = dmrSmsPresetCount();
	if (total == 0) { return; }
	for (int tries = 0; tries < DMR_SMS_NUM_PRESETS; tries++)
	{
		s_msg.presetIdx = (int8_t)((s_msg.presetIdx + 1) % DMR_SMS_NUM_PRESETS);
		const char *p = dmrSmsPresetGet(s_msg.presetIdx);
		if (p != NULL)
		{
			int cap = dmrSmsMaxLen();
			memset(s_msg.compose, 0, sizeof s_msg.compose);
			strncpy(s_msg.compose, p, cap);
			s_msg.compose[cap] = 0;
			s_msg.composePos = (int16_t)strlen(s_msg.compose);
			if (s_msg.composePos > cap) { s_msg.composePos = cap; }
			keyboardResetAlpha();
			composeUpdate();
			return;
		}
	}
}

static void homeEvent(uiEvent_t *ev, menuStatus_t *ec)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, 3);
		*ec |= MENU_STATUS_LIST_TYPE; homeUpdate(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, 3);
		*ec |= MENU_STATUS_LIST_TYPE; homeUpdate(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		keypadAlphaEnable = false;
		keypadAlphaRussian = false;
		keyboardResetAlpha();
		menuSystemPopPreviousMenu();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		switch (menuDataGlobal.currentItemIndex)
		{
			case 0: openFolder(0);  break;
			case 1: openFolder(1);  break;
			case 2: startCompose(); break;
		}
		return;
	}
}

/* ============================ LIST ===================================== */
static void listUpdate(void)
{
	char buf[DMR_SMS_TEXT_MAX + 24];
	int count = dmrSmsCount(s_msg.folder);
	displayClearBuf();
	menuDisplayTitle(s_msg.folder ? (char *)TR("Sent", "\316\362\357\360\340\342\353\345\355\355\373\345") : (char *)TR("Inbox", "\302\365\356\344\377\371\350\345"));

	if (count == 0)
	{
		displayPrintCentered(56, (char *)TR("(empty)", "(\357\363\361\362\356)"), FONT_SIZE_3);
		displayPrintCentered(112, (char *)TR("RED:back", "\312\320\300\321\315:\355\340\347\340\344"), FONT_SIZE_1);
		displayRender();
		return;
	}

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(count + 1, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		if (mNum == count)
		{
			snprintf(buf, sizeof buf, "%s", TR("[Delete all]", "[\323\344\340\353\350\362\374 \342\361\270]"));
		}
		else
		{
			const dmrSmsMessage_t *m = dmrSmsGet(s_msg.folder, mNum);
			if (m == NULL) { continue; }
			char txt[DMR_SMS_TEXT_MAX + 1];
			int n = (m->textLen < (int)sizeof txt - 1) ? m->textLen : (int)sizeof txt - 1;
			memcpy(txt, m->text, n); txt[n] = 0;
			char mark = (m->flags & DMR_SMS_FLAG_UNREAD) ? '*' : ' ';
			snprintf(buf, sizeof buf, "%c%lu:%s", mark, (unsigned long)m->peerId, txt);
		}
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}
	displayRender();
}

static void listEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }
	int count = dmrSmsCount(s_msg.folder);

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED)) { gotoHome(); return; }

	if (count == 0) { return; }

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, count + 1); listUpdate(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, count + 1); listUpdate(); return;
	}

	int idx = menuDataGlobal.currentItemIndex;

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		if (idx == count) { dmrSmsDeleteAll(s_msg.folder); }
		else              { dmrSmsDelete(s_msg.folder, idx); }
		int newCount = dmrSmsCount(s_msg.folder);
		menuDataGlobal.numItems = newCount + 1;
		if (menuDataGlobal.currentItemIndex >= menuDataGlobal.numItems)
		{
			menuDataGlobal.currentItemIndex = menuDataGlobal.numItems - 1;
		}
		listUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (idx == count) { return; }
		s_msg.readIdx = (int16_t)idx;
		dmrSmsMarkRead(s_msg.folder, idx);
		s_msg.view = MSG_READ;
		readUpdate();
		return;
	}
}

/* ============================ READ ===================================== */
static void readUpdate(void)
{
	const dmrSmsMessage_t *m = dmrSmsGet(s_msg.folder, s_msg.readIdx);
	displayClearBuf();
	menuDisplayTitle(s_msg.folder ? (char *)TR("Sent", "\316\362\357\360\340\342\353\345\355\355\373\345") : (char *)TR("Inbox", "\302\365\356\344\377\371\350\345"));

	if (m == NULL) { displayRender(); return; }

	char hdr[24];
	snprintf(hdr, sizeof hdr, "%s %lu",
			(m->flags & DMR_SMS_FLAG_OUTGOING) ? TR("To", "\312\356\354\363:") : TR("From", "\316\362:"),
			(unsigned long)m->peerId);
	displayPrintAt(2, 16, hdr, FONT_SIZE_1);

	// 19 chars per line at FONT_SIZE_2 (8px width: 2 + 19*8 = 154 <= 160px, zero right-edge clipping!)
	// 8 lines at 10px line height = 152 chars capacity (fits full 144-char SMS)
	char txt[DMR_SMS_TEXT_MAX + 1];
	int n = (m->textLen < (int)sizeof txt - 1) ? m->textLen : (int)sizeof txt - 1;
	memcpy(txt, m->text, n); txt[n] = 0;

	const int perLine = 19;
	char line[perLine + 1];
	int y = 26;
	for (int i = 0; i < n && y < 106; i += perLine, y += 10)
	{
		int len = ((n - i) < perLine) ? (n - i) : perLine;
		memcpy(line, txt + i, len); line[len] = 0;
		displayPrintAt(2, y, line, FONT_SIZE_2);
	}

	displayPrintCentered(114, (char *)TR("SK2+GRN:del RED:back", "SK2+\307\305\313:\363\344\340\353 \312\320\300\321\315:\355\340\347\340\344"), FONT_SIZE_1);
	displayRender();
}

static void readEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		dmrSmsDelete(s_msg.folder, s_msg.readIdx);
		openFolderAt(s_msg.folder, s_msg.readIdx);
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED) || KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		openFolderAt(s_msg.folder, s_msg.readIdx);
		return;
	}
}

/* ============================ COMPOSE ================================== */
static void composeUpdate(void)
{
	displayClearBuf();
	menuDisplayTitle((char *)TR("New Message", "\315\356\342\356\345 \361\356\356\341\371."));

	// 19 chars per line (FONT_SIZE_2, 8px wide -> 154px <= 160px), 7 visible rows (y=20..80),
	// auto-scrolls by 1 row when n > 7*19 so all 144 chars are visible while typing.
	const int perLine = 19;
	const int maxVisibleRows = 7;
	int n = (int)strlen(s_msg.compose);
	char line[perLine + 1];
	int y = 20;
	if (n == 0)
	{
		displayPrintAt(2, y, "_", FONT_SIZE_2);
	}
	int startOff = (n > (perLine * maxVisibleRows)) ? perLine : 0;
	for (int i = startOff; i < n && y < 90; i += perLine, y += 10)
	{
		int len = ((n - i) < perLine) ? (n - i) : perLine;
		memcpy(line, s_msg.compose + i, len); line[len] = 0;
		displayPrintAt(2, y, line, FONT_SIZE_2);
	}

	char cnt[28];
	const char *langTag = keypadAlphaRussian ? "[\320\323\321]" : "[ENG]";
	snprintf(cnt, sizeof cnt, TR("%s %d/%d #:lang", "%s %d/%d #:\377\347"), langTag, n, dmrSmsMaxLen());
	displayPrintCentered(94, cnt, FONT_SIZE_1);
	if (dmrSmsPresetCount() > 0)
	{
		displayPrintCentered(104, (char *)TR("U:preset  L/D:del", "\302\302\305\320\325:\370\340\341\353 \302\313\305\302\316:\363\344\340\353"), FONT_SIZE_1);
	}
	else
	{
		displayPrintCentered(104, (char *)TR("L/D:del  #:RUS/ENG", "\302\313\305\302\316/\302\315\310\307:\363\344\340\353 #:\377\347\373\352"), FONT_SIZE_1);
	}
	displayPrintCentered(116, (char *)TR("GRN:to  RED:cancel", "\307\305\313:\352\356\354\363  \312\320\300\321\315:\356\362\354\345\355\340"), FONT_SIZE_1);
	displayRender();
}

static void prefillRecipient(void)
{
	uint32_t defDst = 0;
	int defGroup = 1;
	dmrSmsDefaultRecipient(&defDst, &defGroup);

	uint32_t id;
	if (defDst != 0)
	{
		id = defDst & 0x00FFFFFFu;
		s_msg.rcptGroup = defGroup ? 1 : 0;
	}
	else
	{
		id = trxTalkGroupOrPcId & 0x00FFFFFFu;
		s_msg.rcptGroup = ((trxTalkGroupOrPcId >> 24) == PC_CALL_FLAG) ? 0 : 1;
	}
	snprintf(s_msg.rcpt, sizeof s_msg.rcpt, "%lu", (unsigned long)id);
	s_msg.rcptPos = (int16_t)strlen(s_msg.rcpt);
}

static void composeEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	// Handle multi-tap alpha events FIRST (exact equality: bare KEY_MOD_PREVIEW / KEY_MOD_PRESS
	// without KEY_MOD_DOWN/KEY_MOD_UP) so characters like '#' from key '1' never trigger button handlers.
	if (ev->keys.event == KEY_MOD_PREVIEW)
	{
		if (s_msg.composePos < dmrSmsMaxLen())
		{
			s_msg.compose[s_msg.composePos] = ev->keys.key;
			s_msg.compose[s_msg.composePos + 1] = 0;
			composeUpdate();
		}
		return;
	}
	if (ev->keys.event == KEY_MOD_PRESS)
	{
		if (s_msg.composePos < dmrSmsMaxLen())
		{
			s_msg.compose[s_msg.composePos] = ev->keys.key;
			s_msg.compose[s_msg.composePos + 1] = 0;
			s_msg.composePos++;
			composeUpdate();
		}
		return;
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		gotoHome(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (strlen(s_msg.compose) == 0) { return; }
		keypadAlphaEnable = false;
		keyboardResetAlpha();
		prefillRecipient();
		s_msg.view = MSG_RECIPIENT;
		recipientUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_HASH))
	{
		keypadAlphaRussian = !keypadAlphaRussian;
		keyboardResetAlpha();
		composeUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT) || KEYCHECK_SHORTUP(ev->keys, KEY_DOWN))
	{
		int n = (int)strlen(s_msg.compose);
		if (n > 0)
		{
			s_msg.compose[n - 1] = 0;
			s_msg.composePos = (int16_t)(n - 1);
		}
		keyboardResetAlpha();
		composeUpdate();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))   // insert a CHIRP-configured quick-text preset
	{
		cyclePreset();
		return;
	}
}

/* ============================ RECIPIENT =============================== */
static void recipientUpdate(void)
{
	char buf[40];
	displayClearBuf();
	menuDisplayTitle((char *)TR("Recipient", "\317\356\353\363\367\340\362\345\353\374"));

	snprintf(buf, sizeof buf, "%s: %s", TR("To", "\312\356\354\363"), s_msg.rcpt);
	displayPrintAt(2, 28, buf, FONT_SIZE_3);
	snprintf(buf, sizeof buf, "%s: %s", currentLanguage->type,
			s_msg.rcptGroup ? currentLanguage->group : currentLanguage->private);
	displayPrintAt(2, 52, buf, FONT_SIZE_2);

	displayPrintCentered(84,  (char *)TR("0-9:id  L:del", "0-9:ID  \302\313\305\302\316:\363\344\340\353"), FONT_SIZE_1);
	displayPrintCentered(96,  (char *)TR("U/D:Group/Private", "\302\302\305\320\325/\302\315\310\307:\303\360\363\357\357\340/\313\350\367\355"), FONT_SIZE_1);
	displayPrintCentered(112, (char *)TR("GRN:send  RED:back", "\307\305\313:\356\362\357\360  \312\320\300\321\315:\355\340\347\340\344"), FONT_SIZE_1);
	displayRender();
}

static void doSend(void)
{
	uint32_t dst = (uint32_t)strtoul(s_msg.rcpt, NULL, 10);
	if (dst == 0 || dst > 0x00FFFFFFu)
	{
		s_msg.result = -7;
		s_msg.view = MSG_RESULT;
		s_msg.resultTicks = 600;
		resultUpdate();
		return;
	}
	s_msg.result = (int8_t)dmrSmsSend(s_msg.compose, dst, s_msg.rcptGroup ? 1 : 0, 0);
	s_msg.view = MSG_RESULT;
	s_msg.resultTicks = 600;
	resultUpdate();
}

static void recipientEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		s_msg.view = MSG_COMPOSE;
		s_msg.composePos = (int16_t)strlen(s_msg.compose);
		keyboardResetAlpha();
		keypadAlphaEnable = true;
		composeUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		doSend(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP) || KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		s_msg.rcptGroup = s_msg.rcptGroup ? 0 : 1; recipientUpdate(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT))
	{
		int n = (int)strlen(s_msg.rcpt);
		if (n > 0) { s_msg.rcpt[n - 1] = 0; }
		recipientUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP_NUMBER(ev->keys))
	{
		int n = (int)strlen(s_msg.rcpt);
		if (n < 8) // 24-bit DMR ID max is 16777215 (8 digits)
		{
			s_msg.rcpt[n] = (char)ev->keys.key;
			s_msg.rcpt[n + 1] = 0;
		}
		recipientUpdate();
		return;
	}
}

/* ============================ RESULT ================================== */
static void resultUpdate(void)
{
	displayClearBuf();
	menuDisplayTitle((char *)currentLanguage->messages);
	if (s_msg.result == 0)
	{
		displayPrintCentered(44, (char *)TR("Message sent", "\316\362\357\360\340\342\353\345\355\356!"), FONT_SIZE_3);
		displayPrintCentered(72, (char *)TR("(keyed TX)", "(\357\345\360\345\344\340\367\340 \342 \375\364\350\360)"), FONT_SIZE_2);
	}
	else
	{
		char buf[24];
		const char *why = TR("error", "\356\370\350\341\352\340");
		switch (s_msg.result)
		{
			case -2: why = TR("TX busy",    "TX \347\340\355\377\362");    break;
			case -3: why = TR("no key",     "\355\345\362 \352\353\376\367\340");   break;
			case -1: why = TR("bad text",   "\356\370. \362\345\352\361\362\340");  break;
			case -4:
			case -5: why = TR("too long",   "\361\353\350\370\352\356\354 \344\353."); break;
			case -6: why = TR("TX blocked", "\347\340\357\360\345\362 TX");   break;
			case -7: why = TR("bad ID",     "\355\345\342\345\360\355. ID");  break;
		}
		displayPrintCentered(44, (char *)TR("Send failed", "\316\370\350\341\352\340 \356\362\357\360."), FONT_SIZE_3);
		snprintf(buf, sizeof buf, "%s (ret %d)", why, s_msg.result);
		displayPrintCentered(72, buf, FONT_SIZE_2);
	}
	displayPrintCentered(112, (char *)TR("any key: back", "\353\376\341\340\377 \352\355.: \355\340\347\340\344"), FONT_SIZE_1);
	displayRender();
}

#endif // ENABLE_DMR_DATA
