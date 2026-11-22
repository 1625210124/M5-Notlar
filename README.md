# M5-Notlar
m5sticks3 için not defteri ve gemini ai

## Kablo şeması

4x4 keypad (8 pinli header) ile M5StickS3'ün üstündeki GPIO header'ı doğrudan, pin pin bağlanır. Keypad pasif bir matristir, güç (3V3/5V/GND) bağlamaya gerek yoktur. Sütun pinleri kod içinde `INPUT_PULLUP` olarak ayarlıdır.

| Sıra | M5StickS3 pini | Keypad pini |
|------|----------------|-------------|
| 1    | G5             | R4          |
| 2    | G4             | R3          |
| 3    | G6             | R2          |
| 4    | G7             | R1          |
| 5    | G43            | C1          |
| 6    | G44            | C2          |
| 7    | G2             | C3          |
| 8    | G3             | C4          |

```
M5StickS3 header:  G5  G4  G6  G7  G43 G44 G2  G3
                    |   |   |   |   |   |   |   |
Keypad header:     R4  R3  R2  R1  C1  C2  C3  C4
```

Keypad tuş numaraları (üzerindeki S1–S16 yazıları):

```
        C1   C2   C3   C4
  R1 :  S1   S2   S3   S4
  R2 :  S5   S6   S7   S8
  R3 :  S9   S10  S11  S12
  R4 :  S13  S14  S15  S16
```

Notlar:
- G43 / G44 aynı zamanda UART0 pinleridir. Arduino IDE'de **USB CDC On Boot: Enabled** seçili olmalı, aksi hâlde açılış logları bu pinlere çıkar.
- Tuşlar S2 yerine S5 gibi çıkıyorsa koddaki `KEYPAD_SWAP` değerini `1` yap.
- Keypad'i farklı yönde takarsan Ayarlar → Keypad menüsünden yönü değiştir.
- Buton eşlemeleri: M5 A butonu = G11, B butonu = G12. Keypad S13 = A, S14 = B gibi davranır.
