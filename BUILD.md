# ビルド手順

## 要件

- Windows 10 / 11
- [MinGW-w64](https://www.mingw-w64.org/)（`g++` を使用します）

## ビルド

リポジトリルートで実行

```powershell
g++ -std=c++17 -O2 -municode -mwindows main.cpp -o contrib_widget.exe -lwinhttp
```

成功すると `contrib_widget.exe` が生成されます

## 初回実行前

`config.txt` の 1 行目を GitHub ユーザー名に書き換えて下さい。

## 注意

- `config.txt` は実行ファイルと同じディレクトリに置く

