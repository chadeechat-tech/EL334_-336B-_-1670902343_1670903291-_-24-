#include "MK64F12.h"
#include <stdint.h>

/* --------------------------------------------------------------------------
 * 01 ตั้งค่าบอร์ดและตัวเลือกการทำงาน
 * -------------------------------------------------------------------------- */

/* ใช้ 1 เฉพาะบอร์ดที่ D8 ต่อกับ PTA0 */
#ifndef SHIELD_D8_USES_PTA0
#define SHIELD_D8_USES_PTA0 0
#endif

#define SHIELD_DISPLAY_INVERTED  0U /* รูปแบบสัญญาณของ 7-segment */
#define SHIELD_DISPLAY_SELF_TEST 1  /* ทดสอบจอก่อนเข้าเมนู */

/* 1 สำหรับ buzzer แบบ active และ 0 สำหรับแบบ passive */
#ifndef BUZZER_ACTIVE
#define BUZZER_ACTIVE 1
#endif

/* ที่อยู่ GPIO สำหรับกรณีที่ MK64F12.h ยังไม่ได้กำหนดชื่อเหล่านี้ */
#ifndef GPIOA
#define GPIOA ((GPIO_Type *)0x400FF000UL)
#endif
#ifndef GPIOB
#define GPIOB ((GPIO_Type *)0x400FF040UL)
#endif
#ifndef GPIOC
#define GPIOC ((GPIO_Type *)0x400FF080UL)
#endif
#ifndef GPIOE
#define GPIOE ((GPIO_Type *)0x400FF100UL)
#endif

/* --------------------------------------------------------------------------
 * 02 ผังขาและฟังก์ชัน GPIO
 * -------------------------------------------------------------------------- */

typedef struct {
    PORT_Type *port;
    GPIO_Type *gpio;
    uint32_t pin;
} Pin;

static const Pin buttons[] = {
    {PORTB, GPIOB,  3}, /* ปุ่ม S1 บน Shield ขา A1 / PTB3 */
    {PORTB, GPIOB, 10}, /* ปุ่ม S2 บน Shield ขา A2 / PTB10 */
    {PORTB, GPIOB, 11}  /* ปุ่ม S3 บน Shield ขา A3 / PTB11 */
};

static const Pin shield_latch = {PORTB, GPIOB, 23}; /* D4 ขา Latch */
static const Pin shield_clock = {PORTC, GPIOC,  3}; /* D7 ขา Clock */

#if SHIELD_D8_USES_PTA0
static const Pin shield_data = {PORTA, GPIOA, 0};  /* D8 ขา Data บอร์ดรุ่นเก่า */
#else
static const Pin shield_data = {PORTC, GPIOC, 12}; /* D8 ขา Data บอร์ด Rev E ขึ้นไป */
#endif

static const Pin green_led = {PORTE, GPIOE, 26}; /* ไฟเขียวติดเมื่อลอจิกเป็น 0 */
static const Pin red_led = {PORTB, GPIOB, 22};   /* ไฟแดงติดเมื่อลอจิกเป็น 0 */

static void Pin_Write(const Pin *p, uint8_t high)
{
    if (high != 0U) {
        p->gpio->PSOR = 1UL << p->pin;
    } else {
        p->gpio->PCOR = 1UL << p->pin;
    }
}

static void Pin_Output(const Pin *p, uint8_t initial)
{
    Pin_Write(p, initial);
    p->port->PCR[p->pin] = PORT_PCR_MUX(1);
    p->gpio->PDDR |= 1UL << p->pin;
}

/* --------------------------------------------------------------------------
 * 03 ตัวนับเวลาและการหน่วงเวลา
 * -------------------------------------------------------------------------- */

void SysTick_Handler(void);

/* เพิ่มครั้งละ 1 มิลลิวินาทีใน SysTick_Handler() */
static volatile uint32_t ticks_ms;

static void delay_ms(uint32_t ms)
{
    uint32_t start = ticks_ms;

    while (ticks_ms - start < ms) {
        __WFI();
    }
}

/* --------------------------------------------------------------------------
 * 04 จอ LCD 16x2 ผ่าน Software I2C
 * -------------------------------------------------------------------------- */

/* LCD ใช้ SDA ที่ D1 / PTC17 และ SCL ที่ D0 / PTC16 */
#define LCD_ADDR      0x27U
#define LCD_RS        0x01U
#define LCD_EN        0x04U
#define LCD_BL        0x08U
#define SDA_MASK      (1UL << 17)
#define SCL_MASK      (1UL << 16)
#define LCD_COLS      16U

/* หน่วงประมาณ 5 ไมโครวินาทีสำหรับจังหวะสัญญาณ I2C */
static void I2C_Delay(void)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = (SystemCoreClock + 199999U) / 200000U;

    while (DWT->CYCCNT - start < cycles) {
    }
}

/* ปล่อยสายให้ตัวต้านทาน pull-up ดึงขึ้น HIGH */
static void I2C_Release(uint32_t pin)
{
    GPIOC->PDDR &= ~pin;
}

static void I2C_Low(uint32_t pin)
{
    GPIOC->PCOR = pin;
    GPIOC->PDDR |= pin;
}

static void I2C_Soft_Init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;
    PORTC->PCR[16] = PORT_PCR_MUX(1) | PORT_PCR_ODE_MASK;
    PORTC->PCR[17] = PORT_PCR_MUX(1) | PORT_PCR_ODE_MASK;

    I2C_Release(SDA_MASK);
    I2C_Release(SCL_MASK);
    I2C_Delay();
}

static void I2C_Start(void)
{
    I2C_Release(SDA_MASK);
    I2C_Release(SCL_MASK);
    I2C_Delay();

    I2C_Low(SDA_MASK);
    I2C_Delay();
    I2C_Low(SCL_MASK);
    I2C_Delay();
}

static void I2C_Stop(void)
{
    I2C_Low(SDA_MASK);
    I2C_Delay();

    I2C_Release(SCL_MASK);
    I2C_Delay();

    I2C_Release(SDA_MASK);
    I2C_Delay();
}

static uint8_t I2C_WriteByte(uint8_t data)
{
    uint8_t i;
    uint8_t ack;

    for (i = 0U; i < 8U; ++i) {
        I2C_Low(SCL_MASK);

        if (data & 0x80U) {
            I2C_Release(SDA_MASK);
        } else {
            I2C_Low(SDA_MASK);
        }

        I2C_Delay();
        I2C_Release(SCL_MASK);
        I2C_Delay();
        data <<= 1;
    }

    I2C_Low(SCL_MASK);
    I2C_Release(SDA_MASK);
    I2C_Delay();

    I2C_Release(SCL_MASK);
    I2C_Delay();

    /* อุปกรณ์ตอบ ACK โดยดึงสาย SDA ลง LOW */
    ack = (GPIOC->PDIR & SDA_MASK) == 0U;

    I2C_Low(SCL_MASK);
    I2C_Delay();
    return ack;
}

static void LCD_ExpanderWrite(uint8_t data)
{
    I2C_Start();

    if (I2C_WriteByte((uint8_t)(LCD_ADDR << 1))) {
        I2C_WriteByte(data | LCD_BL);
    }

    I2C_Stop();
}

static void LCD_Write4Bits(uint8_t data)
{
    LCD_ExpanderWrite(data);
    LCD_ExpanderWrite(data | LCD_EN);
    LCD_ExpanderWrite(data);
}

/* ส่งทีละ 4 บิต โดยส่งครึ่งไบต์บนก่อน */
static void LCD_Send(uint8_t value, uint8_t mode)
{
    LCD_Write4Bits((value & 0xF0U) | mode);
    LCD_Write4Bits((uint8_t)((((uint32_t)value << 4U) & 0xF0U) | mode));
}

static void LCD_Command(uint8_t command)
{
    LCD_Send(command, 0U);
    delay_ms((command == 0x01U || command == 0x02U) ? 3U : 1U);
}

static void LCD_Data(uint8_t data)
{
    LCD_Send(data, LCD_RS);
}

static void LCD_Init(void)
{
    delay_ms(50U);
    LCD_Write4Bits(0x30U);
    delay_ms(5U);
    LCD_Write4Bits(0x30U);
    delay_ms(5U);
    LCD_Write4Bits(0x30U);
    delay_ms(2U);
    LCD_Write4Bits(0x20U);
    delay_ms(2U);

    LCD_Command(0x28U);
    LCD_Command(0x0CU);
    LCD_Command(0x01U);
    LCD_Command(0x06U);
}

static void LCD_SetCursor(uint8_t row, uint8_t col)
{
    uint8_t address = (uint8_t)((row == 0U ? 0x00U : 0x40U) + col);

    LCD_Command((uint8_t)(0x80U | address));
}

static void LCD_PrintPadded(const char *text, uint8_t width)
{
    uint8_t i;

    for (i = 0U; i < width; ++i) {
        LCD_Data((uint8_t)(*text != '\0' ? *text++ : ' '));
    }
}

/* เติมช่องว่างให้ครบ 16 ตัวอักษรเพื่อล้างข้อความเดิม */
static void LCD_Line(uint8_t row, const char *text)
{
    LCD_SetCursor(row, 0U);
    LCD_PrintPadded(text, LCD_COLS);
}

/* เขียนรูปตัวอักษรที่สร้างเองลงหน่วยความจำ CGRAM ของ LCD */
static void LCD_LoadGlyphs(uint8_t slot, const uint8_t glyphs[][8],
                           uint8_t count)
{
    uint8_t i;
    uint8_t row;

    LCD_Command((uint8_t)(0x40U | ((uint32_t)slot << 3U)));

    for (i = 0U; i < count; ++i) {
        for (row = 0U; row < 8U; ++row) {
            LCD_Data(glyphs[i][row]);
        }
    }
}

/* --------------------------------------------------------------------------
 * 05 จอ 7-segment และการส่งข้อมูลให้ 74HC595
 * -------------------------------------------------------------------------- */

#define DISPLAY_BLANK 0xFFFFFFFFUL

/* รหัสเปิดขีดของตัวเลข 0-9 และรหัสเลือกหลักซ้ายไปขวา */
static const uint8_t numeral[] = {
    0xC0U, 0xF9U, 0xA4U, 0xB0U, 0x99U,
    0x92U, 0x82U, 0xF8U, 0x80U, 0x90U
};

static const uint8_t digit_select[] = {0xF1U, 0xF2U, 0xF4U, 0xF8U};

/* เก็บตัวเลขเดือนและวันที่หลักละ 4 บิต ใช้ DISPLAY_BLANK เมื่อต้องดับจอ */
static volatile uint32_t display_value = DISPLAY_BLANK;
static volatile uint8_t shield_display_inverted = SHIELD_DISPLAY_INVERTED;

static void Shift_Delay(void)
{
    /* หน่วง 6 รอบ CPU หรือประมาณ 50 นาโนวินาทีที่ 120 MHz */
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
}

/* ส่งรหัสขีด 8 บิตและรหัสเลือกหลัก 8 บิต แล้วสั่ง Latch */
static void Shield_DisplayWrite(uint8_t segment_bits, uint8_t select_bits)
{
    uint16_t word;
    uint16_t bit;

    if (shield_display_inverted != 0U) {
        segment_bits ^= 0xFFU;
        select_bits ^= 0x0FU;
    }

    word = (uint16_t)(((uint32_t)segment_bits << 8U) | select_bits);
    Pin_Write(&shield_latch, 0U);

    for (bit = 0x8000U; bit != 0U; bit >>= 1U) {
        Pin_Write(&shield_data, (word & bit) != 0U);
        Shift_Delay();
        Pin_Write(&shield_clock, 1U);
        Shift_Delay();
        Pin_Write(&shield_clock, 0U);
    }

    Shift_Delay();
    Pin_Write(&shield_latch, 1U);
}

/* --------------------------------------------------------------------------
 * 06 ปุ่ม S1-S3 คิวเหตุการณ์ และ SysTick Interrupt
 * -------------------------------------------------------------------------- */

#define DEBOUNCE_MS 20U
#define BUTTON_QUEUE_SIZE 8U

enum { S1 = 1U, S2 = 2U, S3 = 4U };

static volatile uint8_t button_queue[BUTTON_QUEUE_SIZE];
static volatile uint8_t button_read, button_write;

/* Interrupt ใส่เหตุการณ์กดปุ่มลงคิว โปรแกรมหลักนำไปใช้ทีละเหตุการณ์ */
static void Button_Push(uint8_t event)
{
    uint8_t next = (button_write + 1U) % BUTTON_QUEUE_SIZE;

    if (next != button_read) {
        button_queue[button_write] = event;
        button_write = next;
    }
}

static uint8_t Button_Pop(void)
{
    uint8_t event;

    if (button_read == button_write) {
        return 0U;
    }

    event = button_queue[button_read];
    button_read = (button_read + 1U) % BUTTON_QUEUE_SIZE;
    return event;
}

/* ทำงานทุก 1 มิลลิวินาที การกดค้างสร้างเหตุการณ์ครั้งเดียว */
void SysTick_Handler(void)
{
    static uint8_t active_digit;
    static uint8_t display_was_active;
    static uint8_t stable_buttons;
    static uint8_t debounce[3];
    uint32_t value = display_value;
    uint8_t i;

    ++ticks_ms;

    /* รีเฟรชจอครั้งละหนึ่งหลัก */
    if (value != DISPLAY_BLANK) {
        uint8_t number = (value >> (4U * (3U - active_digit))) & 0x0FU;

        Shield_DisplayWrite(numeral[number], digit_select[active_digit]);
        active_digit = (active_digit + 1U) % 4U;
        display_was_active = 1U;
    } else if (display_was_active) {
        Shield_DisplayWrite(0xFFU, 0xF0U);
        active_digit = 0U;
        display_was_active = 0U;
    }

    /* ปุ่มเป็น Active low และต้องอ่านค่าใหม่คงที่ 20 มิลลิวินาที */
    for (i = 0U; i < 3U; ++i) {
        const Pin *p = &buttons[i];
        uint8_t mask = (uint8_t)(1U << i);
        uint8_t pressed = (p->gpio->PDIR & (1UL << p->pin)) == 0U;
        uint8_t was_pressed = (stable_buttons & mask) != 0U;

        if (pressed == was_pressed) {
            debounce[i] = 0U;
        } else if (++debounce[i] == DEBOUNCE_MS) {
            debounce[i] = 0U;
            stable_buttons ^= mask;

            if (pressed) {
                Button_Push(mask);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * 07 Buzzer และเสียงแจ้งผล
 * -------------------------------------------------------------------------- */

#define BUZZER_MASK (1UL << 1) /* Buzzer บน Shield ขา D3 / PTA1 */

typedef struct {
    uint16_t hz;
    uint16_t ms;
} Note;

/* เสียงเตือนแบบ S1 ค่า 0 Hz หมายถึงช่วงเงียบ */
static const Note warning_sound[] = {
    {1319U, 110U}, {0U, 60U}, {1760U, 220U}
};

/* เสียงยืมสำเร็จแบบ S3 */
static const Note success_sound[] = {
    {1568U, 80U}, {0U, 45U}, {1568U, 80U}, {0U, 45U},
    {1976U, 120U}, {0U, 60U}, {2349U, 160U}, {0U, 55U},
    {3136U, 270U}
};

static void Buzzer_Init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
    GPIOA->PDDR &= ~BUZZER_MASK;
    GPIOA->PCOR = BUZZER_MASK;
    PORTA->PCR[1] = PORT_PCR_MUX(1);
}

/* ดึง D3 ลง LOW เพื่อเปิดเสียง และปล่อยขาเพื่อปิดเสียง */
static void Buzzer_Set(uint8_t on)
{
    if (on != 0U) {
        GPIOA->PDDR |= BUZZER_MASK;
    } else {
        GPIOA->PDDR &= ~BUZZER_MASK;
    }
}

static void Buzzer_WaitCycles(uint32_t start, uint32_t duration)
{
    while (DWT->CYCCNT - start < duration) {
    }
}

static void PlayNote(Note note)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t duration = (SystemCoreClock / 1000U) * note.ms;

#if !BUZZER_ACTIVE
    uint32_t half_period;
    uint32_t deadline = 0U;
    uint8_t on = 1U;
#endif

#if BUZZER_ACTIVE
    Buzzer_Set(note.hz != 0U);
    Buzzer_WaitCycles(start, duration);
#else
    if (note.hz == 0U) {
        Buzzer_Set(0U);
        Buzzer_WaitCycles(start, duration);
    } else {
        /* สลับเปิดและปิดทุกครึ่งคาบเพื่อสร้างความถี่ของโน้ต */
        half_period = SystemCoreClock / (2UL * note.hz);

        while (deadline < duration) {
            Buzzer_Set(on);
            deadline += half_period;

            if (deadline > duration) {
                deadline = duration;
            }

            Buzzer_WaitCycles(start, deadline);
            on ^= 1U;
        }
    }
#endif

    Buzzer_Set(0U);
}

static void PlaySound(const Note *notes, unsigned int count)
{
    unsigned int i;

    for (i = 0U; i < count; ++i) {
        PlayNote(notes[i]);
    }
}

/* --------------------------------------------------------------------------
 * 08 ข้อมูลหนังสือและฟังก์ชันแสดงผลของแต่ละหน้า
 * -------------------------------------------------------------------------- */

/* ชื่อหนังสือแยกเป็นบรรทัดบนและบรรทัดล่างของ LCD */
static const char *const books[][2] = {
    {"The Little", "Prince"},
    {"The Alchemist", ""},
    {"Animal Farm", ""},
    {"1984", ""},
    {"The Hobbit", ""},
    {"Pride and", "Prejudice"},
    {"The Book Thief", ""},
    {"The Hunger Games", ""},
    {"Little Women", ""},
    {"The Great Gatsby", ""}
};

#define BOOK_COUNT (sizeof books / sizeof books[0])

typedef enum { BROWSE, CONFIRM, SET_DATE, SUCCESS, ALREADY_BORROWED } Screen;

static Screen screen = BROWSE;

/* 0 = ยังว่าง, 1 = ถูกยืมแล้ว ข้อมูลอยู่ใน RAM */
static uint8_t book_is_borrowed[BOOK_COUNT];
static uint8_t book, month, day, thai_page;
static uint32_t page_since, success_since, borrowed_since;

static void ShowBook(void)
{
    uint8_t n = book + 1U;

    LCD_Line(0U, books[book][0]);
    LCD_SetCursor(1U, 0U);
    LCD_PrintPadded(books[book][1], 11U);
    LCD_Data('0' + n / 10U);
    LCD_Data('0' + n % 10U);
    LCD_Data('/');
    LCD_Data('0' + BOOK_COUNT / 10U);
    LCD_Data('0' + BOOK_COUNT % 10U);
}

static void ShowConfirmation(void)
{
    uint8_t n = book + 1U;
    char question[] = "Borrow book 00?";

    question[12] = (char)('0' + n / 10U);
    question[13] = (char)('0' + n % 10U);
    LCD_Line(0U, question);
    LCD_Line(1U, "S2 Yes  S1 No");
}

/* ตัวอักษรไทยสำหรับข้อความกำหนดวันคืน ช่อง 0-3 ใช้ร่วมกันสองหน้า */
static const uint8_t thai_shared[][8] = {
    {4, 12, 0, 14, 1, 1, 2, 12},
    {0, 0, 24, 25, 9, 9, 11, 13},
    {5, 15, 0, 14, 17, 21, 25, 17},
    {0, 0, 25, 25, 11, 21, 17, 17}
};

static const uint8_t thai_first[][8] = {
    {0, 6, 6, 14, 17, 25, 17, 17},
    {0, 0, 14, 17, 21, 21, 25, 17},
    {0, 0, 14, 17, 1, 1, 1, 1}
};

static const uint8_t thai_second[][8] = {
    {4, 12, 0, 25, 25, 9, 11, 13},
    {0, 0, 3, 3, 1, 17, 9, 7},
    {5, 15, 0, 15, 18, 14, 18, 18},
    {0, 0, 14, 17, 3, 5, 17, 31}
};

/* สลับข้อความเพราะ CGRAM เก็บอักขระที่สร้างเองได้ครั้งละ 8 แบบ */
static void ShowThaiPage(void)
{
    static const uint8_t words[][9] = {
        {4, 6, 3, 1, 5, 0, 1, 2, 1},
        {0, 1, 2, 1, 3, 4, 5, 6, 7}
    };
    uint8_t i;

    LCD_Command(0x08U);

    if (thai_page == 0U) {
        LCD_LoadGlyphs(4U, thai_first, 3U);
    } else {
        LCD_LoadGlyphs(4U, thai_second, 4U);
    }

    LCD_SetCursor(0U, 0U);
    for (i = 0U; i < 9U; ++i) {
        LCD_Data(words[thai_page][i]);
    }

    LCD_PrintPadded("", 7U);
    LCD_Command(0x0CU);
    page_since = ticks_ms;
}

/* จัดเดือนและวันที่ลงจอ 7-segment ในรูปแบบ MMDD */
static void ShowDate(void)
{
    display_value = ((uint32_t)(month / 10U) << 12) |
                    ((uint32_t)(month % 10U) << 8) |
                    ((uint32_t)(day / 10U) << 4) |
                    (day % 10U);
}

static void ShowBorrowedNotice(void)
{
    LCD_Line(0U, "Already borrowed");
    LCD_Line(1U, "");
}

/* --------------------------------------------------------------------------
 * 09 รับเหตุการณ์ปุ่มและเปลี่ยนสถานะระบบยืม
 * -------------------------------------------------------------------------- */

#define THAI_PAGE_MS 1500U
#define SUCCESS_MS   2000U
#define BORROWED_MS  3000U

static void HandleButton(uint8_t event)
{
    switch (screen) {
    case BROWSE:
        /* S1 ย้อนเล่ม, S3 ไปเล่มถัดไป, S2 เลือกยืม */
        if (event == S1) {
            book = (book + BOOK_COUNT - 1U) % BOOK_COUNT;
            ShowBook();
        } else if (event == S3) {
            book = (book + 1U) % BOOK_COUNT;
            ShowBook();
        } else if (event == S2) {
            /* หนังสือที่ยืมไปแล้วจะขึ้นข้อความเตือนพร้อมไฟแดงและเสียง S1 */
            if (book_is_borrowed[book] != 0U) {
                screen = ALREADY_BORROWED;
                ShowBorrowedNotice();
                borrowed_since = ticks_ms;
                Pin_Write(&red_led, 0U);
                PlaySound(warning_sound,
                          sizeof warning_sound / sizeof warning_sound[0]);
            } else {
                screen = CONFIRM;
                ShowConfirmation();
            }
        }
        break;

    case CONFIRM:
        /* S1 ยกเลิก, S2 ยืนยันเพื่อเริ่มตั้งวันคืน */
        if (event == S1) {
            screen = BROWSE;
            ShowBook();
        } else if (event == S2) {
            month = 1U;
            day = 1U;
            thai_page = 0U;
            screen = SET_DATE;

            LCD_LoadGlyphs(0U, thai_shared, 4U);
            ShowDate();
            LCD_Line(1U, "S1+M S2+D S3 OK");
            ShowThaiPage();
        }
        break;

    case SET_DATE:
        /* S1 เพิ่มเดือน, S2 เพิ่มวันที่, S3 ยืนยันและบันทึกการยืม */
        if (event == S1) {
            month = (uint8_t)(month == 12U ? 1U : month + 1U);
            ShowDate();
        } else if (event == S2) {
            day = (uint8_t)(day == 31U ? 1U : day + 1U);
            ShowDate();
        } else if (event == S3) {
            /* บันทึกสถานะเมื่อผู้ใช้ยืนยันวันคืน แล้วเปิดไฟเขียวพร้อมเสียง S3 */
            book_is_borrowed[book] = 1U;
            screen = SUCCESS;
            LCD_Line(0U, "Loan confirmed");
            LCD_Line(1U, "");

            success_since = ticks_ms;
            Pin_Write(&green_led, 0U);
            PlaySound(success_sound,
                      sizeof success_sound / sizeof success_sound[0]);
        }
        break;

    case SUCCESS:
    case ALREADY_BORROWED:
        break;
    }
}

/* ตรวจเวลาเพื่อสลับข้อความหรือดับไฟและกลับหน้าเลือกหนังสือ */
static void UpdateScreen(void)
{
    if (screen == ALREADY_BORROWED) {
        uint32_t elapsed = ticks_ms - borrowed_since;

        if (elapsed >= BORROWED_MS) {
            Pin_Write(&red_led, 1U);
            screen = BROWSE;
            ShowBook();
        }
    } else if (screen == SET_DATE) {
        if (ticks_ms - page_since >= THAI_PAGE_MS) {
            thai_page ^= 1U;
            ShowThaiPage();
        }
    } else if (screen == SUCCESS &&
               ticks_ms - success_since >= SUCCESS_MS) {
        Pin_Write(&green_led, 1U);
        display_value = DISPLAY_BLANK;
        screen = BROWSE;
        ShowBook();
    }
}

/* --------------------------------------------------------------------------
 * 10 ตั้งค่าอุปกรณ์และลูปหลัก
 * -------------------------------------------------------------------------- */

static void Board_Init(void)
{
    uint8_t i;

    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK | SIM_SCGC5_PORTC_MASK |
                  SIM_SCGC5_PORTE_MASK;

#if SHIELD_D8_USES_PTA0
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
#endif

    /* ตั้งปุ่มทั้งสามเป็นอินพุตและเปิดตัวต้านทานดึงขึ้น */
    for (i = 0U; i < 3U; ++i) {
        const Pin *p = &buttons[i];

        p->gpio->PDDR &= ~(1UL << p->pin);
        p->port->PCR[p->pin] = PORT_PCR_MUX(1) |
                               PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;
    }

    Pin_Output(&shield_latch, 1U);
    Pin_Output(&shield_clock, 0U);
    Pin_Output(&shield_data, 0U);
    Pin_Output(&green_led, 1U);
    Pin_Output(&red_led, 1U);
    Shield_DisplayWrite(0xFFU, 0xF0U);
}

int main(void)
{
    uint8_t event;

    SystemCoreClockUpdate();
    Board_Init();
    Buzzer_Init();

    /* เปิดตัวนับรอบ CPU สำหรับจังหวะ I2C และ buzzer */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* ตั้ง Interrupt ให้ทำงานทุก 1 มิลลิวินาที */
    if (SysTick_Config(SystemCoreClock / 1000U) != 0U) {
        for (;;) {
        }
    }

#if SHIELD_DISPLAY_SELF_TEST
    /* แสดงรูปแบบทดสอบของจอทั้งสองแบบตามลำดับ */
    shield_display_inverted = 0U;
    display_value = 0x1234UL;
    delay_ms(2000U);
    display_value = DISPLAY_BLANK;
    delay_ms(100U);

    shield_display_inverted = 1U;
    display_value = 0x5678UL;
    delay_ms(2000U);
    display_value = DISPLAY_BLANK;
    delay_ms(100U);

    shield_display_inverted = SHIELD_DISPLAY_INVERTED;
#endif

    I2C_Soft_Init();
    delay_ms(100U);
    LCD_Init();
    ShowBook();

    /* อ่านปุ่มจากคิว อัปเดตหน้าจอ แล้วรอ Interrupt รอบถัดไป */
    for (;;) {
        event = Button_Pop();

        if (event != 0U) {
            HandleButton(event);
        }

        UpdateScreen();
        __WFI();
    }
}