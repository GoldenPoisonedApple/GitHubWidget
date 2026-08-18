// ============================================================================
// contrib_widget - GitHubコントリビューションカレンダー常駐表示ウィジェット
//
// 設計方針:
//   - 透過方式: SetLayeredWindowAttributes(LWA_COLORKEY) によるカラーキー透過
//     -> UpdateLayeredWindow + 事前乗算アルファ合成は採用しない。
//        本ウィジェットは矩形の単色塗り潰しのみで半透明合成を必要としないため、
//        DIBセクションの手動管理コストを払う理由がない。
//   - データ取得: GitHub GraphQL API(要トークン)は使用しない。
//     https://github.com/users/{username}/contributions の非公式HTML断片を
//     認証なしGETで取得し、data-date + data-level を正規表現で抽出する。
//     -> トークン管理という不要な複雑性・セキュリティ露出を排除する設計判断。
//        ただし非公開APIであるため、構造変更・レート制限のリスクは残る。
//     (旧実装は data-level の出現順を「週0の日0-6...」とみなしていたが、
//      HTMLは行=曜日(Sun→Sat)・列=週の表構造のため日付ベース配置に変更した。)
//   - 格子配置: 行0=日曜、右端列=今週。data-date から (週列, 曜日行) を算出する。
//   - 未来日: 明日以降(date > today)は描画せず透過。今日までは level 0 含め表示。
//   - 表示週数: config.txt 2行目で直近 n 週に限定(デフォルト10)。右端を今週として左へ n 週分。
//   - ドラッグ移動: WM_NCHITTEST で HTCAPTION を返し、DWM既定の移動処理に委譲。
//     手動マウス追跡(WM_LBUTTONDOWN/WM_MOUSEMOVE)より実装・実行コストが低い。
//     操作ボタン(緑=更新・赤=終了)領域のみ HTCLIENT に戻し、クリックを受け付ける。
//   - 終了手段: 右上の赤ボタン。タスクバー非表示(WS_EX_TOOLWINDOW)のため明示的UIが必要。
//   - 位置永続化: config.txt 3-4行目に終了時の座標を保存し、次回起動時に復元する。
//   - Zオーダー: WS_EX_TOPMOST は使わない。他ウィンドウの背面に回る通常の常駐ウィジェットとする。
//   - 更新中表示: 非同期取得中は更新ボタン左に水色インジケータを出し、無反応と誤認を防ぐ。
//
// ビルド方法は BUILD.md 参照。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <regex>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>

#pragma comment(lib, "winhttp.lib")

using ContributionMap = std::unordered_map<int, int>; // key=YYYYMMDD, value=data-level(0-4)

// ----------------------------------------------------------------------------
// 定数
// ----------------------------------------------------------------------------
constexpr int CELL_SIZE      = 11;   // 1マスの一辺(px)
constexpr int CELL_GAP       = 3;    // マス間の間隔(px)
constexpr int GRID_WEEKS     = 53;   // GitHubカレンダーの週数(最大)
constexpr int GRID_DAYS      = 7;    // 1週間の日数
constexpr COLORREF COLOR_KEY = RGB(1, 1, 1); // 透過色キー。描画内容側では絶対に使用しない色を選ぶこと。
constexpr UINT WM_CONTRIB_UPDATED = WM_APP + 1; // 取得スレッドからUIスレッドへの更新通知
constexpr UINT TIMER_ID_REFRESH   = 1;
constexpr UINT TIMER_INTERVAL_MS  = 60 * 60 * 1000; // 1時間。非公式APIへの過度なポーリングを避ける。
constexpr int DEFAULT_DISPLAY_WEEKS = 10; // config.txt 2行目省略時の表示週数。53週全表示は画面占有が大きいためデフォルトを絞る。
constexpr COLORREF COLOR_BTN_REFRESH = RGB(0, 180, 0);   // 更新ボタン。LEVEL_COLORS/COLOR_KEY と衝突しない純色を選ぶ。
constexpr COLORREF COLOR_BTN_CLOSE   = RGB(255, 0, 0);   // 終了ボタン。視認性のため GitHub 配色とは別系統にする。
constexpr COLORREF COLOR_LOADING     = RGB(135, 206, 235); // 更新中インジケータ(水色)。取得完了まで緑ボタン左に表示する。
constexpr DWORD HTTP_TIMEOUT_MS      = 30000; // WinHTTP 各段階の上限。未設定だと応答待ちで fetchInFlight が降りず水色が残り続ける。

// data-level(0-4) -> 表示色。GitHubデフォルト(ライトテーマ)の配色に準拠。
// 任意の値に変更可能(ダークテーマ配色に合わせる場合はここを書き換える)。
const COLORREF LEVEL_COLORS[5] = {
    RGB(235, 237, 240), // level 0: コントリビューションなし
    RGB(155, 233, 168), // level 1
    RGB(64, 196, 99),   // level 2
    RGB(48, 161, 78),   // level 3
    RGB(33, 110, 57),   // level 4: 最高頻度
};

// ----------------------------------------------------------------------------
// アプリケーション状態
// ----------------------------------------------------------------------------
struct AppConfig {
    std::wstring username;
    int displayWeeks = DEFAULT_DISPLAY_WEEKS;
    int windowX = 0;
    int windowY = 0;
    bool hasSavedPosition = false; // 3-4行目が有効な整数として読めた場合のみ true。未保存時は右上デフォルトを使う。
};

struct AppState {
    std::mutex dataMutex;
    ContributionMap levelsByDate; // 日付(YYYYMMDD) -> level(0-4)。描画時に週×曜日へ配置する。
    std::wstring username;
    int displayWeeks = DEFAULT_DISPLAY_WEEKS; // 描画・ウィンドウ幅・ヒットテストで共有する表示週数
    int fetchInFlight = 0; // 進行中の非同期取得数。UIスレッドのみ更新。>0 の間、水色インジケータを表示する。
};

AppState g_state;

// ----------------------------------------------------------------------------
// 設定ファイル入出力
//
// config.txt 形式:
//   1行目: GitHubユーザー名(必須)
//   2行目: 表示週数 n(省略時 DEFAULT_DISPLAY_WEEKS、1〜GRID_WEEKS にクランプ)
//   3行目: ウィンドウ X(省略可。終了時に自動書き込み)
//   4行目: ウィンドウ Y(省略可。終了時に自動書き込み)
// PAT等の認証情報は不要な設計のため、ユーザー名以外は表示・配置に関する項目のみ。
// ----------------------------------------------------------------------------
void TrimLineEnd(std::wstring& line) {
    // 改行コードCRLFの'\r'混入対策。Windows のテキストエディタ互換のため。
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) {
        line.pop_back();
    }
}

bool LoadConfig(AppConfig& config) {
    std::wifstream f(L"config.txt");
    if (!f.is_open()) return false;

    if (!std::getline(f, config.username)) return false;
    TrimLineEnd(config.username);
    if (config.username.empty()) return false;

    std::wstring line;
    if (std::getline(f, line)) {
        TrimLineEnd(line);
        if (!line.empty()) {
            try {
                // 週数は API 取得上限(53)を超えないようクランプ。過大値はウィンドウ幅肥大化を防ぐ。
                int n = std::stoi(line);
                config.displayWeeks = std::clamp(n, 1, GRID_WEEKS);
            } catch (...) {
                // パース失敗時は DEFAULT_DISPLAY_WEEKS のまま。起動不能にしない。
            }
        }
    }

    std::wstring lineX, lineY;
    if (std::getline(f, lineX) && std::getline(f, lineY)) {
        TrimLineEnd(lineX);
        TrimLineEnd(lineY);
        if (!lineX.empty() && !lineY.empty()) {
            try {
                config.windowX = std::stoi(lineX);
                config.windowY = std::stoi(lineY);
                config.hasSavedPosition = true;
            } catch (...) {
                // 座標行が壊れていても起動は続行。デフォルト位置にフォールバックする。
            }
        }
    }

    return true;
}

bool SaveWindowPosition(int x, int y, int displayWeeks, const std::wstring& username) {
    // 終了時のみ呼ぶ。ドラッグ中の逐次書き込みは I/O 負荷とファイル破損リスクを避けるため行わない。
    std::vector<std::wstring> lines;
    {
        std::wifstream f(L"config.txt");
        if (f.is_open()) {
            std::wstring line;
            while (std::getline(f, line)) {
                lines.push_back(line);
            }
        }
    }

    if (lines.empty()) lines.push_back(username);
    else lines[0] = username;

    if (lines.size() < 2) lines.push_back(std::to_wstring(displayWeeks));
    else lines[1] = std::to_wstring(displayWeeks);

    while (lines.size() < 3) lines.push_back(L"");
    while (lines.size() < 4) lines.push_back(L"");

    lines[2] = std::to_wstring(x);
    lines[3] = std::to_wstring(y);

    std::wofstream out(L"config.txt");
    if (!out.is_open()) return false;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << L"\n";
    }
    return true;
}

// ----------------------------------------------------------------------------
// 作業ディレクトリを exe 配置フォルダへ合わせる。
// スタートアップやショートカット起動時に cwd が System32 等になり config.txt が
// 見つからない問題を防ぐ。config.txt は常に exe と同じ場所を参照する設計とする。
// ----------------------------------------------------------------------------
void SetWorkingDirectoryToExe() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return;
    *slash = L'\0';
    SetCurrentDirectoryW(path);
}

// ----------------------------------------------------------------------------
// レイアウト計算
// 描画・ヒットテスト・CreateWindowExW で同じ式を使い、座標ずれを防ぐ。
// ----------------------------------------------------------------------------
int GetCellPitch() {
    return CELL_SIZE + CELL_GAP;
}

int GetWindowContentWidth(int displayWeeks) {
    // 幅は格子 n 週分のみ。ボタンは格子幅内の最右2列位置に上段へ重ねる(横に伸ばさない)。
    return displayWeeks * GetCellPitch();
}

int GetWindowContentHeight() {
    // 上段1行(操作ボタン) + 格子7日分。ボタン行と格子行を分離して視認性を確保する。
    return (GRID_DAYS + 1) * GetCellPitch();
}

int GetGridOriginY() {
    // 格子はボタン行の直下から開始。y=0 は操作ボタン専用行として空けておく。
    return GetCellPitch();
}

void GetControlButtonRects(int displayWeeks, RECT& greenRect, RECT& redRect) {
    int pitch = GetCellPitch();
    // 格子最右2列と同じ x 座標に [緑][赤] を配置し、上段右寄せの見た目にする。
    int greenCol = std::max(0, displayWeeks - 2);
    int redCol   = std::max(0, displayWeeks - 1);
    greenRect = { greenCol * pitch, 0, greenCol * pitch + CELL_SIZE, CELL_SIZE };
    redRect   = { redCol * pitch, 0, redCol * pitch + CELL_SIZE, CELL_SIZE };
}

void GetLoadingIndicatorRect(int displayWeeks, RECT& loadingRect) {
    int pitch = GetCellPitch();
    // 緑ボタンの1列左。並行取得時も位置を固定し、ユーザーが「更新中」を認識しやすくする。
    int loadingCol = std::max(0, displayWeeks - 3);
    loadingRect = { loadingCol * pitch, 0, loadingCol * pitch + CELL_SIZE, CELL_SIZE };
}

// ----------------------------------------------------------------------------
// WinHTTPによるHTTPS GET
//
// 引数:
//   host   - 接続先ホスト名 (例: L"github.com")
//   path   - リクエストパス (例: L"/users/octocat/contributions")
// 戻り値:
//   レスポンスボディ(UTF-8のまま std::string で保持。パース側でマルチバイトのまま正規表現走査する)
// ----------------------------------------------------------------------------
std::string HttpsGet(const std::wstring& host, const std::wstring& path) {
    std::string response;

    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY: システム設定のプロキシを自動検出して使用。
    // プロキシ環境を考慮せず直結固定でよい場合は WINHTTP_ACCESS_TYPE_NO_PROXY を指定する。
    HINTERNET hSession = WinHttpOpen(
        L"contrib_widget/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return response;

    // タイムアウト未設定時は WinHTTP が長時間ブロックし、UI側の fetchInFlight が降りない。
    WinHttpSetTimeouts(hSession, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return response;
    }

    // WINHTTP_FLAG_SECURE: TLS(HTTPS)を強制。GitHubはHTTP平文接続を受け付けないため必須。
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return response;
    }

    // User-Agent未設定のリクエストは 403 で弾かれる場合があるため明示的に付与する。
    const wchar_t* headers = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) contrib_widget\r\n";

    BOOL sent = WinHttpSendRequest(
        hRequest,
        headers, (DWORD)-1,   // -1: headers を Null終端文字列として長さ自動計算
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD available = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0) break;
            std::vector<char> buf(available);
            DWORD downloaded = 0;
            if (WinHttpReadData(hRequest, buf.data(), available, &downloaded)) {
                response.append(buf.data(), downloaded);
            } else {
                break;
            }
        } while (available > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// ----------------------------------------------------------------------------
// 日付ユーティリティ (ローカル日付。GitHub プロフィール表示と同じ前提)
// YYYYMMDD 整数キーで map と比較・加算を単純化する。
// ----------------------------------------------------------------------------
int YmdFromIso(const std::string& iso) {
    if (iso.size() < 10) return 0;
    return std::stoi(iso.substr(0, 4)) * 10000
         + std::stoi(iso.substr(5, 2)) * 100
         + std::stoi(iso.substr(8, 2));
}

SYSTEMTIME YmdToSystemTime(int ymd) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(ymd / 10000);
    st.wMonth = static_cast<WORD>((ymd / 100) % 100);
    st.wDay = static_cast<WORD>(ymd % 100);
    return st;
}

int SystemTimeToYmd(const SYSTEMTIME& st) {
    return static_cast<int>(st.wYear) * 10000
         + static_cast<int>(st.wMonth) * 100
         + static_cast<int>(st.wDay);
}

int GetTodayYmd() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return SystemTimeToYmd(st);
}

int AddDays(int ymd, int days) {
    SYSTEMTIME st = YmdToSystemTime(ymd);
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart += static_cast<ULONGLONG>(days) * 864000000000ULL;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME out{};
    FileTimeToSystemTime(&ft, &out);
    return SystemTimeToYmd(out);
}

int GetSundayOfWeek(int ymd) {
    SYSTEMTIME st = YmdToSystemTime(ymd);
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    SYSTEMTIME local{};
    FileTimeToSystemTime(&ft, &local);
    // wDayOfWeek: 0=日曜。GitHub 格子の最上段(行0)と揃えるため日曜起点で週を切る。
    return AddDays(ymd, -static_cast<int>(local.wDayOfWeek));
}

// ----------------------------------------------------------------------------
// data-date + data-level を同一 td 要素から抽出する。
// 出現順はHTML内での日付昇順(古い日付が先)であり、これをそのまま
// 「週インデックス優先(週0の日0-6, 週1の日0-6, ...)」の格納順として扱う。
// -> 上記仮定は HTML 表構造(行=曜日)と一致しないため、日付キー map へ変更した。
// ----------------------------------------------------------------------------
bool ParseContributions(const std::string& html, ContributionMap& outLevels) {
    outLevels.clear();
    static const std::regex reDateFirst("data-date=\"(\\d{4}-\\d{2}-\\d{2})\"[^>]*data-level=\"(\\d)\"");
    static const std::regex reLevelFirst("data-level=\"(\\d)\"[^>]*data-date=\"(\\d{4}-\\d{2}-\\d{2})\"");

    auto ingest = [&outLevels](const std::smatch& m, bool dateFirst) {
        int ymd = dateFirst ? YmdFromIso(m[1].str()) : YmdFromIso(m[2].str());
        if (ymd == 0) return;
        int level = std::stoi(dateFirst ? m[2].str() : m[1].str());
        level = std::clamp(level, 0, 4);
        outLevels[ymd] = level;
    };

    for (auto it = std::sregex_iterator(html.begin(), html.end(), reDateFirst); it != std::sregex_iterator(); ++it) {
        ingest(*it, true);
    }
    for (auto it = std::sregex_iterator(html.begin(), html.end(), reLevelFirst); it != std::sregex_iterator(); ++it) {
        ingest(*it, false);
    }
    return !outLevels.empty();
}

// ----------------------------------------------------------------------------
// バックグラウンドスレッドで実行するフェッチ処理。
// UIスレッドをブロックしないよう std::thread で分離し、完了後に
// PostMessage で結果ポインタをUIスレッドへ引き渡す(GDI呼び出しはUIスレッド側でのみ行う)。
// ----------------------------------------------------------------------------
void NotifyFetchCompleted(HWND hwnd, ContributionMap* result) {
    if (!IsWindow(hwnd)) {
        delete result;
        return;
    }
    // PostMessage 失敗時も fetchInFlight を必ず降ろす。失敗しないと水色が点灯し続ける。
    if (!PostMessage(hwnd, WM_CONTRIB_UPDATED, 0, reinterpret_cast<LPARAM>(result))) {
        delete result;
        SendMessage(hwnd, WM_CONTRIB_UPDATED, 0, 0);
    }
}

void FetchContributionsAsync(HWND hwnd, std::wstring username) {
    // UIスレッド上で開始を記録し、取得完了(WM_CONTRIB_UPDATED)まで水色インジケータを点灯する。
    g_state.fetchInFlight++;
    InvalidateRect(hwnd, nullptr, FALSE);

    std::thread([hwnd, username]() {
        auto* result = new ContributionMap();
        try {
            std::wstring path = L"/users/" + username + L"/contributions";
            std::string html = HttpsGet(L"github.com", path);
            if (!html.empty()) {
                ParseContributions(html, *result);
            }
        } catch (...) {
            // パース例外等でも UI 側へ完了通知は必ず送る。通知漏れはインジケータ残留の直接原因になる。
            result->clear();
        }
        NotifyFetchCompleted(hwnd, result);
    }).detach();
}

// ----------------------------------------------------------------------------
// 描画: 直近 displayWeeks 週の格子 + 操作ボタンをクライアント領域に塗る。
// 背景はウィンドウクラスの hbrBackground(COLOR_KEY色)で WM_ERASEBKGND 側にて塗り潰し済み。
// levelsByDate を日付から (週列, 曜日行) へ配置。右端列=今週、行0=日曜。
// ----------------------------------------------------------------------------
void PaintGrid(HDC hdc, const ContributionMap& levelsByDate, int displayWeeks, bool showLoading) {
    int pitch = GetCellPitch();
    int gridOriginY = GetGridOriginY();
    const int today = GetTodayYmd();
    const int currentWeekSunday = GetSundayOfWeek(today);
    const int startSunday = AddDays(currentWeekSunday, -(displayWeeks - 1) * 7);

    for (int w = 0; w < displayWeeks; ++w) {
        for (int d = 0; d < GRID_DAYS; ++d) {
            const int cellDate = AddDays(startSunday, w * 7 + d);
            if (cellDate > today) {
                // 明日以降は GitHub と同様に空(透過)。COLOR_KEY のまま描画しない。
                continue;
            }

            int level = 0;
            const auto it = levelsByDate.find(cellDate);
            if (it != levelsByDate.end()) {
                level = it->second;
            }

            const int x = w * pitch;
            const int y = gridOriginY + d * pitch;
            RECT cell{ x, y, x + CELL_SIZE, y + CELL_SIZE };

            HBRUSH brush = CreateSolidBrush(LEVEL_COLORS[level]);
            FillRect(hdc, &cell, brush);
            DeleteObject(brush);
        }
    }

    RECT loadingRect;
    GetLoadingIndicatorRect(displayWeeks, loadingRect);
    if (showLoading) {
        HBRUSH loadingBrush = CreateSolidBrush(COLOR_LOADING);
        FillRect(hdc, &loadingRect, loadingBrush);
        DeleteObject(loadingBrush);
    } else {
        // InvalidateRect(..., FALSE) では WM_ERASEBKGND が走らず、前フレームの水色が残る。
        // 取得完了後は COLOR_KEY で明示的に上書きし、透過領域として消す。
        HBRUSH keyBrush = CreateSolidBrush(COLOR_KEY);
        FillRect(hdc, &loadingRect, keyBrush);
        DeleteObject(keyBrush);
    }

    RECT greenRect, redRect;
    GetControlButtonRects(displayWeeks, greenRect, redRect);
    HBRUSH greenBrush = CreateSolidBrush(COLOR_BTN_REFRESH);
    FillRect(hdc, &greenRect, greenBrush);
    DeleteObject(greenBrush);
    HBRUSH redBrush = CreateSolidBrush(COLOR_BTN_CLOSE);
    FillRect(hdc, &redRect, redBrush);
    DeleteObject(redBrush);
}

// ----------------------------------------------------------------------------
// ウィンドウプロシージャ
// ----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        // WS_EX_LAYERED + LWA_COLORKEY: hbrBackgroundで塗った COLOR_KEY 部分を
        // OSコンポジタが透過処理する。UpdateLayeredWindowと異なりDIB手動管理は不要。
        SetLayeredWindowAttributes(hwnd, COLOR_KEY, 0, LWA_COLORKEY);

        // 起動直後に初回フェッチ、以後は TIMER_INTERVAL_MS 間隔で再取得。
        FetchContributionsAsync(hwnd, g_state.username);
        SetTimer(hwnd, TIMER_ID_REFRESH, TIMER_INTERVAL_MS, nullptr);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == TIMER_ID_REFRESH) {
            FetchContributionsAsync(hwnd, g_state.username);
        }
        return 0;
    }

    case WM_CONTRIB_UPDATED: {
        auto* newLevels = reinterpret_cast<ContributionMap*>(lParam);
        if (newLevels != nullptr) {
            if (!newLevels->empty()) {
                std::lock_guard<std::mutex> lock(g_state.dataMutex);
                g_state.levelsByDate = *newLevels;
            }
            delete newLevels;
        }
        // 取得成功/失敗に関わらずインジケータを消灯。空結果でも再描画しないと水色が残る。
        if (g_state.fetchInFlight > 0) {
            g_state.fetchInFlight--;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        const bool showLoading = g_state.fetchInFlight > 0;
        {
            std::lock_guard<std::mutex> lock(g_state.dataMutex);
            PaintGrid(hdc, g_state.levelsByDate, g_state.displayWeeks, showLoading);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST: {
        // 操作ボタンは HTCLIENT のままにし WM_LBUTTONUP を届ける。
        // ボタン行の空白や格子領域は HTCAPTION にしてドラッグ移動を可能にする。
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT greenRect, redRect;
        GetControlButtonRects(g_state.displayWeeks, greenRect, redRect);
        if (PtInRect(&greenRect, pt) || PtInRect(&redRect, pt)) {
            return HTCLIENT;
        }
        LRESULT hit = DefWindowProc(hwnd, msg, wParam, lParam);
        if (hit == HTCLIENT) return HTCAPTION;
        return hit;
    }

    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT greenRect, redRect;
        GetControlButtonRects(g_state.displayWeeks, greenRect, redRect);
        if (PtInRect(&redRect, pt)) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (PtInRect(&greenRect, pt)) {
            FetchContributionsAsync(hwnd, g_state.username);
            return 0;
        }
        break;
    }

    case WM_DESTROY: {
        RECT rc;
        GetWindowRect(hwnd, &rc);
        SaveWindowPosition(rc.left, rc.top, g_state.displayWeeks, g_state.username);
        KillTimer(hwnd, TIMER_ID_REFRESH);
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// エントリポイント
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {

    SetWorkingDirectoryToExe();

    AppConfig config;
    if (!LoadConfig(config)) {
        MessageBoxW(nullptr,
            L"config.txt が見つからないか、1行目にGitHubユーザー名が記述されていません。\n"
            L"実行ファイルと同じディレクトリに config.txt を作成してください。\n"
            L"  1行目: GitHubユーザー名(必須)\n"
            L"  2行目: 表示週数(省略時10)\n"
            L"  3-4行目: ウィンドウ位置 X,Y(省略可。終了時に自動保存)",
            L"contrib_widget", MB_ICONERROR);
        return 1;
    }

    g_state.username = config.username;
    g_state.displayWeeks = config.displayWeeks;

    const wchar_t* className = L"ContribWidgetClass";

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COLOR_KEY); // WM_ERASEBKGND既定処理でCOLOR_KEY一色に塗る
    wc.lpszClassName = className;

    if (!RegisterClassExW(&wc)) return 1;

    int width  = GetWindowContentWidth(g_state.displayWeeks);
    int height = GetWindowContentHeight();

    int x, y;
    if (config.hasSavedPosition) {
        x = config.windowX;
        y = config.windowY;
    } else {
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        x = screenW - width - 20; // 初回のみ画面右上、端から20px内側
        y = 20;
    }

    // dwExStyle:
    //   WS_EX_LAYERED    - レイヤードウィンドウ化(透過処理の前提)
    //   WS_EX_TOOLWINDOW - タスクバー・Alt+Tab一覧から除外
    // WS_EX_TOPMOST は付与しない。他ウィンドウの背面に回る通常 Z オーダーとする。
    // dwStyle:
    //   WS_POPUP のみを指定し、WS_CAPTION/WS_THICKFRAME/WS_SYSMENU を含めない
    //   -> タイトルバー・システムメニュー・サイズ変更枠が一切描画されない
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        className, L"", WS_POPUP,
        x, y, width, height,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}