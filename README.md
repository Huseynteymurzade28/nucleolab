# NucleoLab: NUCLEO-L452RE üzerinde SCPI laboratuvar cihazı

Osiloskop (1 MS/s, 12 bit), sinyal jeneratörü, kart üstü FFT (THD/SNR) ve Bode analizörü. Bare-metal C (CMSIS), kendi SCPI çözümleyicimiz.

**Girişler yalnızca 0–3.3 V.** PA4 bir çıkıştır; başka bir çıkışa bağlama.

Kart: **NUCLEO-L452RE-P** (STM32L452RET6P). Düz NUCLEO-L452RE için aşağıdaki pin notlarına bakın.

## Kurulum
    git clone --recursive https://github.com/Huseynteymurzade28/nucleolab
    cd nucleolab
    python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
    make flash        # derle ve yükle
    ./run.sh          # panel: http://127.0.0.1:8080, SCPI: 127.0.0.1:5025

## Bağlantı
- Osiloskop: PA4 - A0 (PA0) jumper. -P kartta PA4 A2 pininde değil, morpho header'dadır. Jumper yoksa dahili kanal 3 kullanılır.
- Bode: PA4 - 10 kΩ - PA0, PA0 - 10 nF - GND (teorik kesim 1.59 kHz).
  1 kΩ kullanma: DAC tamponu en az 5 kΩ yük ister.

## Demo (3 dakika)
1. `./run.sh`, paneli aç: canlı 1 kHz sinüs. Dalga tipini ve frekansı değiştir.
2. Spektrum: önce sinüs, sonra kare dalga (tek harmonikler görünür).
3. RC filtreyi tak, kanal 1, Bode taraması: ölçüm teori eğrisiyle karşılaştırılır.
4. Başka terminalde `.venv/bin/python host/demo_pyvisa.py`: panel "UZAKTAN KONTROL"e geçer.
5. Konsola `MAKE:COFFEE` yaz: -113 hatası, cihaz çalışmaya devam eder.

## Çok kanallı voltmetre (otomatik algılama)
Kanal *n* = header pini A(*n*−1). Pinler **NUCLEO-L452RE-P** (MB1319) kartına göre ayarlı. Düz NUCLEO-L452RE kartında A2 = PA4, A3 = PB0 olur, o durumda `fw/board/analog.c` içindeki `chmap` değiştirilmeli.

| Kanal | Pin | | Kanal | Pin |
|---|---|---|---|---|
| 1 | A0 (PA0) | | 4 | A3 (PC2) |
| 2 | A1 (PA1) | | 5 | A4 (PC1) |
| 3 | A2 (PC3) | | 6 | A5 (PC0) |

Bu kartta jeneratör çıkışı PA4 A2'de değil, morpho header'dadır. Osiloskop kanalları (`ACQ:CHAN 1–3`) eskisi gibidir: 1 = PA0, 2 = PA1, 3 = PA4 (dahili).

**Uyarı:** -P çipinde (STM32L452RETxP) PC5 ve PD2 GPIO değildir, harici SMPS çekirdek beslemesine aittir. Bu pinlerin kayıtlarına dokunmak çipi elektrik kesilene kadar kararsız bırakır.

Kart her girişi arka planda sırayla yoklar (~20 ms'de bir kanal). Pin önce dahili pull-up, sonra pull-down ile şarj edilir, ardından ölçülür. Boşta duran pin bu şarjı korur ve iki okuma çok farklı çıkar (~800 kod). Bir şeye bağlı pin ise iki seferde de kendi gerilimini verir (~0 fark). Takma/çıkarma ~100–200 ms içinde fark edilir. Bu durumda `STAT:OPER` bit 9 ayarlanır ve sayaç artar.

| Komut | Yanıt |
|---|---|
| `MEAS:VOLT? (@1,4:6)` | Listelenen kanalların gerilimi (V). Boş kanal `9.91E+37` (SCPI NaN) döner. Parametresiz `MEAS:VOLT?` eskisi gibi osiloskop ölçümü yapar |
| `ROUT:CHAN:CONN? [(@liste)]` | Her kanal için 1 = bağlı, 0 = boş (varsayılan: hepsi) |
| `ROUT:CHAN:CAT?` | Bağlı kanallar, ör. `(@1,4)` |
| `ROUT:CHAN:NAME? [(@liste)]` | `"A0","A1",…` |
| `ROUT:CHAN:CHAN?` | Algılama değişikliği sayacı |
| `DIAG:CHAN:PROB? [(@liste)]` | Ham algılama farkı (eşik 300 kod) |

**TUI** (Odin + [hnoss](https://github.com/Huseynteymurzade28/hnoss)): `make tui && tui/nucleolab-tui`. Köprü (`./run.sh`) açıksa 5025 portuna, değilse doğrudan seri porta bağlanır. Tüm kanalları canlı gösterir, takma/çıkarma olaylarını yazar. Konsoluna yazılan her şey SCPI komutu olarak gönderilir. Tuşlar: Tab kanal seç, ↑↓ komut geçmişi, Esc çık.

## Yapı
| Yol | İçerik |
|---|---|
| `fw/scpi` | Donanımdan bağımsız SCPI çekirdeği |
| `fw/board` | Saat, UART, ADC/DAC/DMA |
| `fw/app` | Komutlar, ölçüm, FFT, Bode |
| `fw/app/dvm.c` | Çok kanallı voltmetre + takma algılama |
| `host` | Köprü, web paneli, PyVISA örneği |
| `tui` | hnoss terminal arayüzü |
| `tests`, `tools` | PC testleri + fuzzing, kart testleri |

## Test (köprü kapalıyken)
    make test
    make board-test
    .venv/bin/python tools/lab_test.py
    .venv/bin/python tools/bode_test.py

Tüm komutların listesi: `SYST:HELP:HEAD?`. Kartın önceki programını geri yüklemek: `make restore`. Bunun için `backup/original_flash.bin` gerekir. Bu dosya repoda yoktur, kendi kartınızdan `st-flash read backup/original_flash.bin 0x08000000 0x80000` ile alın.

## Sınırlar
- DAC ve ADC aynı referansı ve saati paylaşır; kendi kendini ölçmek mutlak doğruluğu kanıtlamaz.
- Anti-alias filtresi yoktur: örneklemenin yarısından yüksek sinyaller yanlış frekansta görünür.
- Bode, test sırasında çıkışı 2 Vpp sinüse alır, sonra önceki ayara döner.
- Köprü yalnızca 127.0.0.1'i dinler, kimlik doğrulaması yoktur.
- DAC çıkışında yumuşatma filtresi yoktur. ADC örneği basamak geçişine denk gelirse SNR düşer (10 kHz, 1 MS/s: medyan 62 dB, en kötü 44 dB).

## Lisans
MIT. `third_party/` altındaki CMSIS dosyaları kendi lisanslarıyla (Apache-2.0) gelir.
