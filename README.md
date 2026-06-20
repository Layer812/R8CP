# RONTO8 for M5Cardputer (v0.4)

![RONTO8 Logo](r8cp.gif)

RONTO8 is an experimental PICO-8 compatible fantasy console emulator specially designed and optimized for the **M5Stack Cardputer**. It is based on [femto8](https://github.com/benbaker76/femto8) and [zepto8](https://github.com/samhocevar/zepto8), bringing the joy of portable PICO-8 gaming and coding to this compact, ESP32-S3-powered device.

RONTO8は、**M5Stack Cardputer** 専用に設計および最適化された PICO-8 互換のファンタジーコンソールエミュレータ（実験バージョン）です。[femto8](https://github.com/benbaker76/femto8) と [zepto8](https://github.com/samhocevar/zepto8) をベースにしており、ESP32-S3を搭載したこのコンパクトなデバイスで、PICO-8のゲームやコーディングの楽しさを持ち歩くことができます。

---

## ⚠️ Disclaimer / 注意事項

- **Experimental Release / 実験的なリリース**: This is highly experimental code. If you find bugs, please report them gently! (実験的なコードであるため、バグを見つけた場合は優しく教えてください！)
- **Memory Limitations / メモリ制限**: Due to the hardware limitations of the M5Cardputer (ESP32-S3), some very large PICO-8 cartridges may not work or may cause "not enough memory" errors during Lua compilation. (Cardputerのハードウェア制限により、非常に大きいカートリッジは動作しないか、コンパイル時にメモリ不足エラーになる場合があります。)

---

## 🌟 Key Features / 主な機能

- **Optimized for M5Cardputer**: Full support for the Cardputer's display, keyboard, and speaker.
  - Cardputerのディスプレイ、キーボード、スピーカーに完全対応。
- **SD Card ROM Browser**: Load `.p8.png` or `.p8` cartridges directly from the SD card.
  - SDカードから `.p8.png` や `.p8` 形式のカートリッジを直接ロード可能。
- **High-speed Emulation**: Tuned Lua compiler and garbage collector to overcome memory limitations of embedded systems.
  - 組み込み環境のメモリ制限を克服するため、Luaコンパイラとガベージコレクションを徹底的にチューニング。
- **Audio Support**: Enhanced audio synthesis for authentic PICO-8 SFX and Music playback.
  - PICO-8特有の効果音（SFX）やBGMを再現するオーディオエンジンを搭載。

## 🎮 Controls / 操作方法

- **D-Pad / 方向キー**: `W`, `A`, `S`, `D`
- **Button O (Z/C)**: `Z` or `N`
- **Button X (X/V)**: `X` or `M`
- **Start / Pause**: `P` or `ENTER`

**In the ROM Browser / ROMブラウザでの操作**:
- **Up**: `W` or `;`
- **Down**: `S` or `.`
- **Select / Open Folder**: `ENTER`

## ⚙️ Setup & Building / ビルド方法

1. Clone the repository:
   ```bash
   git clone https://github.com/benbaker76/femto8.git ronto8
   cd ronto8
   ```
2. Build and upload using PlatformIO:
   ```bash
   pio run --target upload ; pio device monitor
   ```
3. Prepare the SD Card:
   Place your `.p8.png` or `.p8` files on the root or in a folder on your MicroSD card and insert it into the Cardputer.
   - MicroSDカードのルートやフォルダ内に `.p8.png` または `.p8` ファイルを置き、Cardputerに挿入してください。

## 🙏 Credits and Acknowledgments / 謝辞

RONTO8 is heavily based on the incredible work of the open-source community:
- [benbaker76](https://github.com/benbaker76) - Original author and maintainer of [femto8](https://github.com/benbaker76/femto8)
- [Jacopo Santoni](https://github.com/Jakz) - Author of [retro8](https://github.com/Jakz/retro8)
- [Lexaloffle](https://www.lexaloffle.com/) - The visionary creator of the amazing PICO-8 fantasy console.
- **Layer8** - M5Cardputer Port, Audio engine fixes, and memory optimization.
- **Share Code**: pgrbeoPrOeXVnea8

*(C) 2026 Layer8. BASED ON FEMTO8 & ZEPTO8.*
