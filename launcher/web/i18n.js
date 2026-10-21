// 介面翻譯集中管理；不翻譯程式名稱、路徑或客體輸出。
const translations = {
"Sandbox 根憑證": ["Sandbox root certificates", "Sandbox ルート証明書", "Sandbox 루트 인증서"],
"使用 Sandbox 根憑證檔案": ["Use a Sandbox root certificate file", "Sandbox ルート証明書ファイルを使用", "Sandbox 루트 인증서 파일 사용"],
"保存位置（相對於 Sandbox C 槽）": ["Location (relative to Sandbox C:)", "保存場所（Sandbox C: の相対パス）", "저장 위치 (Sandbox C: 기준 상대 경로)"],
"首次啟動時優先沿用既有 ROOT.pem，缺檔才以當下時間與安全亂數生成。開關預設關閉，只控制是否使用此檔案；TLS 仍使用內建公開根憑證。": ["The first launch reuses an existing ROOT.pem or generates one using the current time and secure randomness if missing. The switch is off by default and controls only whether this file is used; TLS still uses embedded public roots.", "初回起動時は既存の ROOT.pem を使用し、ファイルがない場合のみ現在時刻と安全な乱数で生成します。スイッチは既定でオフで、このファイルの使用のみを制御します。TLS は内蔵の公開ルートを引き続き使用します。", "처음 실행할 때 기존 ROOT.pem을 유지하고 파일이 없을 때만 현재 시각과 안전한 난수로 생성합니다. 스위치는 기본적으로 꺼져 있으며 이 파일의 사용 여부만 제어합니다. TLS는 내장 공개 루트를 계속 사용합니다."],
"Windows 介面": ["Windows interface", "Windows インターフェイス", "Windows 인터페이스"],
"Windows 介面字型": ["Windows interface font", "Windows インターフェイスのフォント", "Windows 인터페이스 글꼴"],
"預設向量字型": ["Default scalable font", "既定のベクターフォント", "기본 벡터 글꼴"],
"自動（macOS 系統向量字型）": ["Automatic (macOS system font)", "自動（macOS システムフォント）", "자동 (macOS 시스템 글꼴)"],
"套用於 Windows 系統介面與未指定字型的文字，儲存後於程式下次啟動生效。": ["Used for Windows system interfaces and text without a specified font. Saved changes apply on the next app launch.", "Windows システムの画面とフォント未指定の文字に使用します。保存後、アプリの次回起動時に適用されます。", "Windows 시스템 인터페이스와 글꼴을 지정하지 않은 텍스트에 사용합니다. 저장 후 앱의 다음 실행부터 적용됩니다."],
"字型預覽": ["Font preview", "フォントのプレビュー", "글꼴 미리 보기"],
"請選擇已安裝的向量字型": ["Select an installed scalable font", "インストール済みのベクターフォントを選択してください", "설치된 벡터 글꼴을 선택하세요"],
"此程式 CPU 核心數": ["App CPU cores", "アプリの CPU コア数", "앱 CPU 코어 수"],
"沿用全域設定": ["Use global settings", "全体設定を使用", "전역 설정 사용"],
"獨立保存，於此程式下次啟動生效。": ["Saved per app; applies on its next launch.", "アプリごとに保存し、次回起動時に適用します。", "앱별로 저장하며 다음 실행부터 적용됩니다."],
"原生 V8 模組原型": ["Native V8 module prototype", "ネイティブ V8 モジュール試作", "네이티브 V8 모듈 프로토타입"],
"使用 ARM64 V8 執行橋接測試程式。目前尚未自動替換 NW.js／遊戲內嵌 V8。": ["Runs bridge test apps with ARM64 V8. Does not yet replace V8 embedded in NW.js or games.", "ARM64 V8 でブリッジテストを実行します。NW.js やゲーム内蔵の V8 はまだ置換しません。", "ARM64 V8로 브리지 테스트 앱을 실행합니다. NW.js나 게임에 내장된 V8은 아직 교체하지 않습니다."],
"V8 模組已內建，預設關閉。儲存後於下次啟動生效。": ["V8 module included; off by default. Saved changes apply on the next launch.", "V8 モジュールを同梱。既定では無効です。保存後、次回起動時に適用されます。", "V8 모듈이 포함되어 있으며 기본값은 꺼짐입니다. 저장 후 다음 실행부터 적용됩니다."],
"此版本未包含 ARM64 V8 模組。": ["This build does not include the ARM64 V8 module.", "このビルドには ARM64 V8 モジュールが含まれていません。", "이 빌드에는 ARM64 V8 모듈이 포함되어 있지 않습니다."],
"加入 V8 測試程式": ["Add V8 test apps", "V8 テストアプリを追加", "V8 테스트 앱 추가"],
"儲存開關後，將 x86／x64 測試程式加入程式庫，再按執行圖示測試。結果顯示於終端與 LOG。": ["Save the switch, add the x86/x64 test apps, then click their play icons. Results appear in the terminal and LOG.", "設定を保存し、x86/x64 テストをライブラリに追加して実行ボタンを押します。結果はターミナルと LOG に表示されます。", "설정을 저장하고 x86/x64 테스트 앱을 라이브러리에 추가한 뒤 실행 아이콘을 누르세요. 결과는 터미널과 LOG에 표시됩니다."],
"實驗性功能": ["Experimental features", "実験的な機能", "실험적 기능"],
"原生函式庫橋接層": ["Native library bridge", "ネイティブライブラリブリッジ", "네이티브 라이브러리 브리지"],
"將支援的函式庫呼叫轉交 ARM64 原生實作。目前僅支援部分 zlib 函式。": ["Routes supported library calls to native ARM64 implementations. Currently supports a subset of zlib functions.", "対応するライブラリ呼び出しを ARM64 ネイティブ実装に転送します。現在は一部の zlib 関数に対応しています。", "지원되는 라이브러리 호출을 ARM64 네이티브 구현으로 전달합니다. 현재 일부 zlib 함수만 지원합니다."],
"預設關閉。儲存後套用至所有程式的下次啟動。": ["Off by default. Saved changes apply to all apps on their next launch.", "既定では無効です。保存後、すべてのアプリの次回起動時に適用されます。", "기본값은 꺼짐입니다. 저장한 변경 사항은 모든 앱의 다음 실행부터 적용됩니다."],
"版本 {0}": ["Version {0}", "バージョン {0}", "버전 {0}"],
"開發版本": ["Development build", "開発版", "개발 버전"],
"每次調整 512 MiB，上限為系統實體記憶體的 50%。預設 4096 MiB（4 GiB），按使用量配置，於下次執行生效。": ["Adjust in 512 MiB steps, up to 50% of physical system memory. Default: 4096 MiB (4 GiB), allocated as needed. Applies on the next launch.", "512 MiB 単位で調整し、上限はシステムの物理メモリの 50% です。既定は 4096 MiB（4 GiB）、必要に応じて割り当て、次回起動時に反映されます。", "512 MiB 단위로 조정하며, 시스템 물리 메모리의 50%가 상한입니다. 기본값은 4096 MiB(4 GiB)이며 필요에 따라 할당하고 다음 실행부터 적용됩니다."],
"系統記憶體：{0} MiB · 上限 50%": ["System memory: {0} MiB · Limit: 50%", "システムメモリ：{0} MiB · 上限 50%", "시스템 메모리: {0} MiB · 상한 50%"],
"CPU 執行方式": ["CPU execution", "CPU 実行方式", "CPU 실행 방식"],
"編譯執行緒數": ["Compiler threads", "コンパイルスレッド数", "컴파일 스레드 수"],
"{0} 個執行緒": ["{0} threads", "{0} スレッド", "스레드 {0}개"],
"同步編譯": ["Synchronous compilation", "同期コンパイル", "동기 컴파일"],
"預設 2；0 為同步編譯。儲存後套用至下次程式啟動及加速分析。": ["Default: 2; 0 uses synchronous compilation. Saved values apply to the next app launch and acceleration analysis.", "既定は 2、0 は同期コンパイルです。保存した値は次回のアプリ起動と高速化解析に適用されます。", "기본값은 2이며, 0은 동기 컴파일입니다. 저장한 값은 다음 앱 실행 및 가속 분석에 적용됩니다."],
"編譯執行緒數必須介於 0 與 8": ["Compiler threads must be between 0 and 8", "コンパイルスレッド数は 0～8 にしてください", "컴파일 스레드 수는 0에서 8 사이여야 합니다"],
"宿主實體核心：{0}": ["Host physical cores: {0}", "ホストの物理コア：{0}", "호스트 물리 코어: {0}"],
"{0} 個核心": ["{0} cores", "{0} コア", "코어 {0}개"],
"每次調整 1 個核心，儲存後套用於該架構所有程式的下次啟動。": ["Adjust in steps of 1 core. Saved values apply to all apps of that architecture on the next launch.", "1 コア単位で調整します。保存した値は同じアーキテクチャのすべてのアプリの次回起動時に反映されます。", "코어 1개 단위로 조정합니다. 저장한 값은 해당 아키텍처의 모든 앱에 다음 실행부터 적용됩니다."],
"預設 x64 使用宿主實體核心，x86 使用 1 個核心。": ["Defaults: host physical cores for x64; 1 core for x86.", "既定では x64 はホストの物理コア、x86 は 1 コアを使用します。", "기본값은 x64의 경우 호스트 물리 코어, x86의 경우 코어 1개입니다."],
"核心使用方式由執行引擎依程式架構自動管理。": ["The engine manages CPU cores based on the app's architecture.", "実行エンジンがアプリのアーキテクチャに応じてコアを管理します。", "실행 엔진이 앱 아키텍처에 따라 CPU 코어를 관리합니다."],
"執行模式": ["Execution mode", "実行モード", "실행 모드"],
"Resident AOT 常駐轉譯": ["Resident AOT translation", "Resident AOT 常駐変換", "Resident AOT 상주 변환"],
"Windows x64 核心數": ["Windows x64 cores", "Windows x64 コア数", "Windows x64 코어 수"],
"Windows x86 核心數": ["Windows x86 cores", "Windows x86 コア数", "Windows x86 코어 수"],
"自動使用宿主實體核心": ["Automatic host physical cores", "ホストの物理コアを自動使用", "호스트 물리 코어 자동 사용"],
"1 個核心": ["1 core", "1 コア", "코어 1개"],
"指令數上限": ["Instruction limit", "命令数の上限", "명령어 수 제한"],
"依個別程式設定，預設不限": ["Per-app setting; unlimited by default", "アプリごとの設定、既定は無制限", "앱별 설정, 기본값은 무제한"],
"繪圖後端（所有程式）": ["Graphics backend (all apps)", "描画バックエンド（すべてのアプリ）", "그래픽 백엔드 (모든 앱)"],
"程式 GPU 記憶體": ["App GPU memory", "アプリの GPU メモリ", "앱 GPU 메모리"],
"目前程式：{0}": ["Selected app: {0}", "選択中のアプリ：{0}", "선택한 앱: {0}"],
"先從程式庫選取程式，再調整它的 GPU 記憶體。": ["Select an app from the library to adjust its GPU memory.", "ライブラリからアプリを選択して GPU メモリを調整してください。", "라이브러리에서 앱을 선택한 후 GPU 메모리를 조정하세요."],
"GPU 記憶體只套用於目前選取的程式，其他 EXE 的容量各自保存。": ["GPU memory applies to the selected app. Other EXEs keep their own capacities.", "GPU メモリは選択中のアプリに適用され、他の EXE はそれぞれの容量を保持します。", "GPU 메모리는 선택한 앱에 적용됩니다. 다른 EXE는 각자의 용량을 유지합니다."],
"已儲存。介面偏好已套用，執行設定於下次啟動生效。": ["Saved. Appearance applied; execution settings take effect on the next launch.", "保存しました。外観を適用し、実行設定は次回起動時に反映されます。", "저장했습니다. 화면 설정은 적용되었으며 실행 설정은 다음 실행부터 적용됩니다."],
"即時資源用量": ["Live resource usage", "リアルタイム使用量", "실시간 리소스 사용량"],
"正在取樣…": ["Sampling…", "測定中…", "측정 중…"],
"等待更新…": ["Waiting for an update…", "更新を待っています…", "업데이트 대기 중…"],
"已更新 {0}": ["Updated {0}", "更新 {0}", "업데이트 {0}"],
"處理器": ["Processor", "プロセッサ", "프로세서"],
"圖形處理器": ["Graphics processor", "グラフィックス", "그래픽 프로세서"],
"記憶體": ["Memory", "メモリ", "메모리"],
"網路": ["Network", "ネットワーク", "네트워크"],
"100% = 1 個核心": ["100% = 1 CPU core", "100% = 1 コア", "100% = CPU 코어 1개"],
"模擬程序實際佔用量": ["Emulator process memory", "エミュレーターの実使用量", "에뮬레이터 프로세스 메모리"],
"每 3 秒更新此模擬程序的用量。": ["Updates this emulator process every 3 seconds.", "このプロセスの使用量を 3 秒ごとに更新。", "이 에뮬레이터 프로세스를 3초마다 업데이트합니다."],
"GPU 記憶體 {0} / {1}": ["GPU memory {0} / {1}", "GPU メモリ {0} / {1}", "GPU 메모리 {0} / {1}"],
"此繪圖後端未提供計時 · {0}": ["Timing unavailable · {0}", "時間計測に非対応 · {0}", "GPU 시간 측정 불가 · {0}"],
"執行輸出": ["Execution output", "実行出力", "실행 출력"],
"清除執行輸出": ["Clear execution output", "実行出力を消去", "실행 출력 지우기"],
"執行後會在此顯示程式的標準輸出與錯誤輸出。": ["The app's standard output and error output will appear here when it runs.", "実行すると、アプリの標準出力とエラー出力がここに表示されます。", "실행하면 앱의 표준 출력과 오류 출력이 여기에 표시됩니다."],
"正在等待執行輸出…": ["Waiting for execution output…", "実行出力を待っています…", "실행 출력을 기다리는 중…"],
"尚無執行輸出。": ["No execution output.", "実行出力はありません。", "실행 출력이 없습니다."],
"保留最新 128KB 的標準輸出與錯誤輸出。": ["Keeps the latest 128KB of standard output and error output.", "標準出力とエラー出力の最新 128KB を保持します。", "표준 출력과 오류 출력의 최신 128KB를 보관합니다."],
"已淘汰較舊內容。": ["Older content has been discarded.", "古い内容は削除されました。", "이전 내용은 삭제되었습니다."],
"OpenGL 繪圖後端": ["OpenGL rendering backend", "OpenGL 描画バックエンド", "OpenGL 렌더링 백엔드"],
"auto — 優先 Metal，初始化失敗時使用舊版 OpenGL": ["auto — Prefer Metal; use legacy OpenGL if initialization fails", "auto — Metal を優先し、初期化失敗時は従来の OpenGL", "auto — Metal 우선, 초기화 실패 시 기존 OpenGL 사용"],
"metal — 直接 Metal 硬體加速": ["metal — Direct Metal hardware acceleration", "metal — Metal による直接ハードウェア描画", "metal — 직접 Metal 하드웨어 가속"],
"legacy — 舊版 OpenGL driver": ["legacy — Legacy OpenGL driver", "legacy — 従来の OpenGL ドライバー", "legacy — 기존 OpenGL 드라이버"],
"套用於所有程式的 OpenGL，於下次啟動生效。需要 Metal 尚未支援的 OpenGL 功能時，請選 legacy。": ["Applies to OpenGL in all apps on their next launch. Choose legacy for OpenGL features not yet supported by Metal.", "すべてのアプリの OpenGL に次回起動時から適用されます。Metal 未対応の OpenGL 機能が必要な場合は legacy を選択してください。", "모든 앱의 OpenGL에 다음 실행부터 적용됩니다. Metal이 아직 지원하지 않는 OpenGL 기능은 legacy를 선택하세요."],
"Direct3D 9／11 使用 Metal，GPU 記憶體沿用所選程式的設定。": ["Direct3D 9/11 uses Metal and the selected app's GPU memory setting.", "Direct3D 9／11 は Metal を使用し、選択中のアプリの GPU メモリ設定に従います。", "Direct3D 9/11은 Metal을 사용하며 선택한 앱의 GPU 메모리 설정을 따릅니다."],
"不支援的 OpenGL 後端": ["Unsupported OpenGL backend", "未対応の OpenGL バックエンド", "지원하지 않는 OpenGL 백엔드"],
  " · {0} 個執行中": [
    " · {0} running",
    " · {0} 件実行中",
    " · {0}개 실행 중"
  ],
  " · {0} 個臨時程式": [
    " · {0} temporary",
    " · 一時実行 {0} 件",
    " · 임시 실행 {0}개"
  ],
  " · 正在建立快取": [
    " · Building cache",
    " · キャッシュ作成中",
    " · 캐시 생성 중"
  ],
  " · 選取程式，開始執行。": [
    " · Select an app to start.",
    " · アプリを選択して実行。",
    " · 앱을 선택하여 실행하세요."
  ],
  ": 對應目錄": [
    ": mapped folder",
    ": マッピング先",
    ": 연결 폴더"
  ],
  "C 槽映射": [
    "C: drive mapping",
    "C: ドライブの割り当て",
    "C: 드라이브 매핑"
  ],
  "C: 必填": [
    "C: required",
    "C: 必須",
    "C: 필수"
  ],
  "C: 為必要設定。可新增 D: 至 Z:，供各程式在下次啟動時使用；讀寫權限沿用各 EXE 的設定。": [
    "C: is required. Add drives D: through Z: for future launches. Each EXE keeps its own write permissions.",
    "C: は必須です。D:〜Z: を追加すると、次回起動時に使用されます。書き込み権限は各 EXE の設定に従います。",
    "C:는 필수입니다. D:부터 Z:까지 추가하면 다음 실행부터 사용됩니다. 쓰기 권한은 각 EXE 설정을 따릅니다."
  ],
  "Ctrl C 中斷": [
    "Ctrl C interrupt",
    "Ctrl C 中断",
    "Ctrl C 중단"
  ],
  "Ctrl D 結束輸入": [
    "Ctrl D end input",
    "Ctrl D 入力終了",
    "Ctrl D 입력 종료"
  ],
  "GPU 記憶體必須是 16–8192 MiB 的整數": [
    "GPU memory must be an integer from 16 to 8192 MiB.",
    "GPU メモリは 16〜8192 MiB の整数で指定してください。",
    "GPU 메모리는 16~8192 MiB의 정수여야 합니다."
  ],
  "GPU 記憶體（MiB）": [
    "GPU memory (MiB)",
    "GPU メモリ（MiB）",
    "GPU 메모리 (MiB)"
  ],
  "WINDOWS，從這裡開始": [
    "WINDOWS STARTS HERE",
    "WINDOWS をここから",
    "WINDOWS, 여기서 시작"
  ],
  "Windows 程式": [
    "Windows apps",
    "Windows アプリ",
    "Windows 앱"
  ],
  "{0} / {1} 個檔案{2}": [
    "{0} / {1} files{2}",
    "{0} / {1} ファイル{2}",
    "파일 {0} / {1}개{2}"
  ],
  "{0} 個程式{1}{2}": [
    "{0} apps{1}{2}",
    "{0} 個のアプリ{1}{2}",
    "앱 {0}개{1}{2}"
  ],
  "{0}，詳細原因可查看「執行紀錄」。": [
    "{0}. See the run log for details.",
    "{0}。詳細は実行ログをご確認ください。",
    "{0}. 자세한 내용은 실행 로그를 확인하세요."
  ],
  "一般": [
    "General",
    "一般",
    "일반"
  ],
  "主控台程式在內嵌終端互動，GUI 程式開啟獨立視窗。": [
    "Console apps use the embedded terminal; GUI apps open in their own windows.",
    "コンソールアプリは内蔵ターミナルで、GUI アプリは独立したウィンドウで実行されます。",
    "콘솔 앱은 내장 터미널에서, GUI 앱은 별도 창에서 실행됩니다."
  ],
  "互動式終端": [
    "Interactive terminal",
    "対話型ターミナル",
    "대화형 터미널"
  ],
  "介面配色": [
    "Color theme",
    "配色テーマ",
    "색상 테마"
  ],
  "以獨立視窗開啟": [
    "Opens in its own window",
    "独立したウィンドウで開く",
    "별도 창에서 열기"
  ],
  "你可以回到這裡查看狀態或停止程式。": [
    "Return here to check the status or stop the app.",
    "ここで状態を確認したり、アプリを停止したりできます。",
    "여기에서 상태를 확인하거나 앱을 중지할 수 있습니다."
  ],
  "你的 Windows 程式庫": [
    "Your Windows app library",
    "Windows アプリライブラリ",
    "나의 Windows 앱 라이브러리"
  ],
  "例如：--help 或 -c .": [
    "For example: --help or -c .",
    "例：--help または -c .",
    "예: --help 또는 -c ."
  ],
  "依程式支援的 Windows 命令列格式填寫，可保留空白與引號。": [
    "Use the app’s Windows command-line syntax, including spaces and quotes.",
    "アプリが対応する Windows コマンドライン形式で入力してください。空白と引用符も使用できます。",
    "앱이 지원하는 Windows 명령줄 형식을 사용하세요. 공백과 따옴표를 사용할 수 있습니다."
  ],
  "保留原始 EXE 與設定檔": [
    "Keep the EXE and its settings",
    "元の EXE と設定を保持",
    "원본 EXE와 설정 유지"
  ],
  "停止": [
    "Stop",
    "停止",
    "중지"
  ],
  "停止程式": [
    "Stop app",
    "アプリを停止",
    "앱 중지"
  ],
  "儲存設定": [
    "Save settings",
    "設定を保存",
    "설정 저장"
  ],
  "允許程式寫入上述資料夾": [
    "Allow the app to write to these folders",
    "これらのフォルダーへの書き込みを許可",
    "이 폴더에 앱의 쓰기 허용"
  ],
  "切換臨時工作階段": [
    "Switch temporary session",
    "一時セッションを切り替え",
    "임시 세션 전환"
  ],
  "加入 EXE": [
    "Add EXE",
    "EXE を追加",
    "EXE 추가"
  ],
  "加入 EXE（⌘ O）": [
    "Add EXE (⌘ O)",
    "EXE を追加（⌘ O）",
    "EXE 추가 (⌘ O)"
  ],
  "加入 Windows x86／x64 執行檔，選取後即可執行。": [
    "Add a Windows x86/x64 executable, then select it to run.",
    "Windows x86／x64 実行ファイルを追加し、選択して実行できます。",
    "Windows x86/x64 실행 파일을 추가하고 선택하여 실행하세요."
  ],
  "加入第一個 EXE，": [
    "Add your first EXE",
    "最初の EXE を追加して、",
    "첫 EXE를 추가하고"
  ],
  "加入第一個 EXE，開始建立你的程式庫。": [
    "Add your first EXE to build your library.",
    "最初の EXE を追加してライブラリを作成しましょう。",
    "첫 EXE를 추가하여 라이브러리를 만드세요."
  ],
  "取消": [
    "Cancel",
    "キャンセル",
    "취소"
  ],
  "取消分析": [
    "Cancel analysis",
    "解析を中止",
    "분석 취소"
  ],
  "可直接輸入文字；切換程式時，終端內容會保留。": [
    "Type directly here. Terminal content is kept when switching apps.",
    "ここに直接入力できます。アプリを切り替えてもターミナルの内容は保持されます。",
    "여기에 직접 입력하세요. 앱을 전환해도 터미널 내용은 유지됩니다."
  ],
  "啟動參數": [
    "Launch arguments",
    "起動引数",
    "실행 인수"
  ],
  "啟動參數、工作資料夾與 C 槽映射可在「程式設定」調整。": [
    "Change launch arguments, working folder and C: mapping in App settings.",
    "起動引数、作業フォルダー、C: の割り当ては「アプリ設定」で変更できます。",
    "실행 인수, 작업 폴더 및 C: 매핑은 앱 설정에서 변경하세요."
  ],
  "啟動失敗：{0}": [
    "Launch failed: {0}",
    "起動失敗：{0}",
    "실행 실패: {0}"
  ],
  "啟動設定": [
    "Launch settings",
    "起動設定",
    "실행 설정"
  ],
  "在 Apple Silicon 上執行 Windows x86／x64 程式，提供程式庫、獨立啟動設定與互動式終端。": [
    "Run Windows x86/x64 apps on Apple Silicon with an app library, per-app settings and an interactive terminal.",
    "Apple Silicon 上で Windows x86／x64 アプリを実行。ライブラリ、アプリ別の設定、対話型ターミナルを提供します。",
    "Apple Silicon에서 Windows x86/x64 앱을 실행하세요. 라이브러리, 앱별 설정 및 대화형 터미널을 제공합니다."
  ],
  "在 Finder 顯示 ↗": [
    "Show in Finder ↗",
    "Finder で表示 ↗",
    "Finder에서 보기 ↗"
  ],
  "執行": [
    "Run",
    "実行",
    "실행"
  ],
  "執行中": [
    "Running",
    "実行中",
    "실행 중"
  ],
  "執行失敗": [
    "Failed",
    "実行失敗",
    "실행 실패"
  ],
  "執行引擎已就緒": [
    "Engine ready",
    "エンジン準備完了",
    "엔진 준비 완료"
  ],
  "執行狀態": [
    "Run status",
    "実行状態",
    "실행 상태"
  ],
  "執行程式": [
    "Run app",
    "アプリを実行",
    "앱 실행"
  ],
  "執行紀錄 ↗": [
    "Run log ↗",
    "実行ログ ↗",
    "실행 로그 ↗"
  ],
  "多國語言": [
    "Language",
    "言語",
    "언어"
  ],
  "套用設定": [
    "Apply settings",
    "設定を適用",
    "설정 적용"
  ],
  "安裝位置：{0}": [
    "Install location: {0}",
    "インストール先：{0}",
    "설치 위치: {0}"
  ],
  "安裝目錄（選填）": [
    "Install folder (optional)",
    "インストール先（任意）",
    "설치 폴더 (선택)"
  ],
  "完成": [
    "Done",
    "完了",
    "완료"
  ],
  "客體程式輸出顯示在終端；這裡保留執行引擎的診斷。": [
    "App output appears in the terminal. Engine diagnostics are shown here.",
    "アプリの出力はターミナルに表示されます。ここにはエンジンの診断情報を表示します。",
    "앱 출력은 터미널에 표시됩니다. 여기에는 엔진 진단 정보가 표시됩니다."
  ],
  "尚未執行": [
    "Not started",
    "未実行",
    "실행 전"
  ],
  "工作資料夾": [
    "Working folder",
    "作業フォルダー",
    "작업 폴더"
  ],
  "已停止": [
    "Stopped",
    "停止済み",
    "중지됨"
  ],
  "已儲存，於下次啟動程式時套用。": [
    "Saved. Drive changes apply on the next launch.",
    "保存しました。ドライブの変更は次回起動時に適用されます。",
    "저장했습니다. 드라이브 변경은 다음 실행부터 적용됩니다."
  ],
  "已加入 {0} 個程式": [
    "Added {0} apps",
    "{0} 個のアプリを追加しました",
    "앱 {0}개 추가됨"
  ],
  "已結束": [
    "Exited",
    "終了",
    "종료됨"
  ],
  "已結束，退出碼 {0}": [
    "exited with code {0}",
    "終了（終了コード {0}）",
    "종료됨 (종료 코드 {0})"
  ],
  "引擎執行紀錄": [
    "Engine run log",
    "エンジン実行ログ",
    "엔진 실행 로그"
  ],
  "從列表移除": [
    "Remove from list",
    "一覧から削除",
    "목록에서 제거"
  ],
  "從列表移除程式會保留原始 EXE 與設定檔。": [
    "Removing an app from the list keeps its EXE and settings.",
    "一覧から削除しても、元の EXE と設定ファイルは保持されます。",
    "목록에서 앱을 제거해도 원본 EXE와 설정 파일은 유지됩니다."
  ],
  "找不到原始 EXE，請確認磁碟已連接，或重新加入檔案。": [
    "EXE not found. Connect its drive or add the file again.",
    "元の EXE が見つかりません。ドライブを接続するか、ファイルを再度追加してください。",
    "원본 EXE를 찾을 수 없습니다. 드라이브를 연결하거나 파일을 다시 추가하세요."
  ],
  "找不到執行引擎": [
    "Engine not found",
    "エンジンが見つかりません",
    "엔진을 찾을 수 없음"
  ],
  "指定資料夾對應到程式內的 C:\\ 環境。": [
    "Map this folder to the app’s C:\\ drive.",
    "指定フォルダーをアプリの C:\\ ドライブに割り当てます。",
    "이 폴더를 앱의 C:\\ 드라이브에 연결합니다."
  ],
  "按下「執行」後，程式視窗會顯示在桌面。": [
    "Press Run to open the app on your desktop.",
    "「実行」を押すと、デスクトップにアプリが表示されます。",
    "실행을 누르면 바탕 화면에 앱 창이 표시됩니다."
  ],
  "按下「執行」，在這裡輸入並查看程式輸出。": [
    "Press Run, then type and view output here.",
    "「実行」を押すと、ここで入力と出力を確認できます。",
    "실행을 누른 후 여기에서 입력하고 출력을 확인하세요."
  ],
  "搜尋程式": [
    "Search apps",
    "アプリを検索",
    "앱 검색"
  ],
  "新加入的程式會使用進階頁設定的預設 C:。更改預設值不會覆寫既有 EXE 的個別設定。": [
    "New apps use the default C: from Advanced settings. Changing defaults preserves existing per-app settings.",
    "新しいアプリには「詳細設定」の既定の C: が使用されます。既定値を変更しても、既存のアプリ設定は上書きされません。",
    "새 앱은 고급 설정의 기본 C:를 사용합니다. 기본값을 변경해도 기존 앱별 설정은 유지됩니다."
  ],
  "日本語": [
    "日本語",
    "日本語",
    "日本語"
  ],
  "是否現在分析這個程式？": [
    "Analyze this app now?",
    "このアプリを今すぐ解析しますか？",
    "지금 이 앱을 분석할까요?"
  ],
  "暖琥珀": [
    "Warm amber",
    "アンバー",
    "따뜻한 호박색"
  ],
  "本次臨時執行{0}，可關閉此工作階段。": [
    "Temporary run {0}. You can close this session.",
    "一時実行は{0}。このセッションを閉じられます。",
    "임시 실행 {0}. 이 세션을 닫을 수 있습니다."
  ],
  "本次設定不會加入程式庫，關閉後清除。": [
    "These temporary settings are cleared when closed.",
    "この一時設定はライブラリに保存されず、閉じると消去されます。",
    "임시 설정은 라이브러리에 저장되지 않으며 닫으면 지워집니다."
  ],
  "本次設定已套用": [
    "Temporary settings applied",
    "一時設定を適用しました",
    "임시 설정 적용됨"
  ],
  "查看分析進度": [
    "View analysis progress",
    "解析の進行状況",
    "분석 진행 상황"
  ],
  "森林綠": [
    "Forest green",
    "フォレストグリーン",
    "숲의 초록"
  ],
  "檔案不存在": [
    "File not found",
    "ファイルがありません",
    "파일 없음"
  ],
  "正在停止…": [
    "Stopping…",
    "停止中…",
    "중지 중…"
  ],
  "正在準備…": [
    "Preparing…",
    "準備中…",
    "준비 중…"
  ],
  "正在準備引擎…": [
    "Preparing engine…",
    "エンジン準備中…",
    "엔진 준비 중…"
  ],
  "正在確認分析範圍": [
    "Checking analysis scope",
    "解析範囲を確認中",
    "분석 범위 확인 중"
  ],
  "此程式的設定已儲存": [
    "App settings saved",
    "アプリ設定を保存しました",
    "앱 설정 저장됨"
  ],
  "每個 EXE 使用自己的設定檔，下次啟動時套用。": [
    "Each EXE has its own settings, applied on the next launch.",
    "各 EXE の設定は個別に保存され、次回起動時に適用されます。",
    "각 EXE의 설정은 개별 저장되며 다음 실행부터 적용됩니다."
  ],
  "每個 EXE 分別保存 C:、工作目錄與啟動參數，可在主畫面的「程式設定」調整。": [
    "Each EXE stores its own C:, working folder and arguments. Edit them in App settings.",
    "各 EXE の C:、作業フォルダー、起動引数は個別に保存されます。「アプリ設定」で変更できます。",
    "각 EXE의 C:, 작업 폴더 및 실행 인수는 개별 저장됩니다. 앱 설정에서 변경할 수 있습니다."
  ],
  "每個 EXE 的 C: 與工作目錄仍可獨立設定。更改映射不會搬移或刪除目錄中的檔案。": [
    "Each EXE can override C: and its working folder. Mapping changes never move or delete files.",
    "各 EXE の C: と作業フォルダーは個別に設定できます。割り当てを変更してもファイルは移動・削除されません。",
    "각 EXE의 C:와 작업 폴더는 개별 설정할 수 있습니다. 매핑을 변경해도 파일은 이동되거나 삭제되지 않습니다."
  ],
  "每個程式，各自保存啟動設定。": [
    "Launch settings saved for each app.",
    "アプリごとに起動設定を保存。",
    "앱마다 실행 설정을 저장합니다."
  ],
  "沒有符合的程式。": [
    "No matching apps.",
    "一致するアプリがありません。",
    "일치하는 앱이 없습니다."
  ],
  "海洋藍": [
    "Ocean blue",
    "オーシャンブルー",
    "바다 파랑"
  ],
  "深色": [
    "Dark",
    "ダーク",
    "어둡게"
  ],
  "清除": [
    "Clear",
    "消去",
    "지우기"
  ],
  "清除畫面": [
    "Clear screen",
    "画面を消去",
    "화면 지우기"
  ],
  "清除終端畫面": [
    "Clear terminal",
    "ターミナルを消去",
    "터미널 지우기"
  ],
  "準備分析": [
    "Preparing analysis",
    "解析準備",
    "분석 준비"
  ],
  "準備就緒": [
    "Ready",
    "準備完了",
    "준비됨"
  ],
  "無法更新執行狀態：{0}": [
    "Unable to update status: {0}",
    "状態を更新できません：{0}",
    "상태를 갱신할 수 없음: {0}"
  ],
  "熟悉的程式，新的起點。": [
    "Familiar apps. A new beginning.",
    "いつものアプリを、新しい環境で。",
    "익숙한 앱, 새로운 시작."
  ],
  "獨立設定檔": [
    "App settings file",
    "個別設定ファイル",
    "개별 설정 파일"
  ],
  "目前關閉寫入，安裝程式無法建立檔案。安裝前請開啟寫入。": [
    "Writing is disabled. Enable it before installing so the installer can create files.",
    "書き込みが無効です。インストール前に有効にしてください。",
    "쓰기가 비활성화되어 있습니다. 설치 프로그램이 파일을 만들 수 있도록 설치 전에 활성화하세요."
  ],
  "磁碟代號": [
    "Drive letter",
    "ドライブ文字",
    "드라이브 문자"
  ],
  "磁碟代號不可重複": [
    "Drive letters must be unique.",
    "ドライブ文字は重複できません。",
    "드라이브 문자는 중복될 수 없습니다."
  ],
  "移出列表": [
    "Remove from list",
    "一覧から削除",
    "목록에서 제거"
  ],
  "移出列表會保留原始 EXE 與設定檔。": [
    "Removing from the list keeps the EXE and its settings.",
    "一覧から削除しても元の EXE と設定は保持されます。",
    "목록에서 제거해도 원본 EXE와 설정은 유지됩니다."
  ],
  "移除": [
    "Remove",
    "削除",
    "제거"
  ],
  "程式以此資料夾讀取或寫入檔案。": [
    "The app reads and writes files relative to this folder.",
    "アプリはこのフォルダーを基準にファイルを読み書きします。",
    "앱은 이 폴더를 기준으로 파일을 읽고 씁니다."
  ],
  "程式功能": [
    "App features",
    "アプリ機能",
    "앱 기능"
  ],
  "程式加速分析": [
    "Analyze for faster launch",
    "起動高速化の解析",
    "실행 속도 분석"
  ],
  "程式加速分析進度": [
    "Launch analysis progress",
    "起動高速化の解析状況",
    "실행 속도 분석 진행 상황"
  ],
  "程式可能在安裝目錄內建立自己的子資料夾。": [
    "The app may create its own subfolder here.",
    "アプリがこの中に独自のサブフォルダーを作成する場合があります。",
    "앱이 이 폴더 안에 하위 폴더를 만들 수 있습니다."
  ],
  "程式在獨立的桌面視窗中執行。": [
    "The app is running in its own desktop window.",
    "アプリは独立したデスクトップウィンドウで実行中です。",
    "앱이 별도의 바탕 화면 창에서 실행 중입니다."
  ],
  "程式已停止，可重新啟動。": [
    "App stopped. You can start it again.",
    "アプリは停止しました。再起動できます。",
    "앱이 중지되었습니다. 다시 실행할 수 있습니다."
  ],
  "程式已結束，退出碼 {0}。再次按下「執行」可重新啟動。": [
    "App exited with code {0}. Press Run to start again.",
    "終了コード {0} で終了しました。「実行」で再起動できます。",
    "앱이 코드 {0}으로 종료되었습니다. 실행을 눌러 다시 시작하세요."
  ],
  "程式庫": [
    "Library",
    "ライブラリ",
    "라이브러리"
  ],
  "程式庫與啟動設定": [
    "Library and launch settings",
    "ライブラリと起動設定",
    "라이브러리 및 실행 설정"
  ],
  "程式正在獨立視窗執行": [
    "App running in its own window",
    "独立したウィンドウで実行中",
    "별도 창에서 앱 실행 중"
  ],
  "程式設定": [
    "App settings",
    "アプリ設定",
    "앱 설정"
  ],
  "程式項目": [
    "App item",
    "アプリ項目",
    "앱 항목"
  ],
  "稍後再說": [
    "Later",
    "後で",
    "나중에"
  ],
  "紫羅蘭": [
    "Violet",
    "バイオレット",
    "보라색"
  ],
  "終端": [
    "Terminal",
    "ターミナル",
    "터미널"
  ],
  "終端已準備好": [
    "Terminal ready",
    "ターミナル準備完了",
    "터미널 준비 완료"
  ],
  "繁體中文": [
    "繁體中文",
    "繁體中文",
    "繁體中文"
  ],
  "臨時 · ": [
    "Temporary · ",
    "一時 · ",
    "임시 · "
  ],
  "臨時執行": [
    "Run once",
    "一時実行",
    "임시 실행"
  ],
  "臨時工作階段": [
    "Temporary sessions",
    "一時セッション",
    "임시 세션"
  ],
  "自動（依系統語言）": [
    "Automatic (system language)",
    "自動（システム言語）",
    "자동 (시스템 언어)"
  ],
  "設定": [
    "Settings",
    "設定",
    "설정"
  ],
  "設定分頁": [
    "Settings tabs",
    "設定タブ",
    "설정 탭"
  ],
  "請先停止程式再移除": [
    "Stop the app before removing it.",
    "削除する前にアプリを停止してください。",
    "제거하기 전에 앱을 중지하세요."
  ],
  "進階": [
    "Advanced",
    "詳細設定",
    "고급"
  ],
  "選取 {0}": [
    "Select {0}",
    "{0} を選択",
    "{0} 선택"
  ],
  "選取程式，開始執行。": [
    "Select an app to start.",
    "アプリを選択して実行。",
    "앱을 선택하여 실행하세요."
  ],
  "選擇 C 槽資料夾": [
    "Choose C: folder",
    "C: フォルダーを選択",
    "C: 폴더 선택"
  ],
  "選擇…": [
    "Choose…",
    "選択…",
    "선택…"
  ],
  "選擇安裝目錄": [
    "Choose install folder",
    "インストール先を選択",
    "설치 폴더 선택"
  ],
  "選擇工作資料夾": [
    "Choose working folder",
    "作業フォルダーを選択",
    "작업 폴더 선택"
  ],
  "選擇後立即預覽，儲存後於下次開啟時保留。": [
    "Preview changes immediately. Save to keep them for next time.",
    "変更をすぐにプレビューできます。保存すると次回も適用されます。",
    "변경 사항이 즉시 미리 표시됩니다. 저장하면 다음에도 유지됩니다."
  ],
  "選擇或輸入目錄": [
    "Choose or enter a folder",
    "フォルダーを選択または入力",
    "폴더 선택 또는 입력"
  ],
  "選擇磁碟對應目錄": [
    "Choose mapped folder",
    "割り当て先フォルダーを選択",
    "매핑할 폴더 선택"
  ],
  "開始分析": [
    "Start analysis",
    "解析を開始",
    "분석 시작"
  ],
  "開始建立你的程式庫。": [
    "and start building your library.",
    "ライブラリを作成しましょう。",
    "라이브러리를 만드세요."
  ],
  "關於": [
    "About",
    "このアプリについて",
    "정보"
  ],
  "關閉": [
    "Close",
    "閉じる",
    "닫기"
  ],
  "關閉執行紀錄": [
    "Close run log",
    "実行ログを閉じる",
    "실행 로그 닫기"
  ],
  "關閉後清除臨時項目，安裝與輸出檔案會保留。": [
    "Closing removes the temporary entry and keeps installed and output files.",
    "閉じると一時項目が消去されます。インストール済みファイルと出力は保持されます。",
    "닫으면 임시 항목이 제거됩니다. 설치 및 출력 파일은 유지됩니다."
  ],
  "關閉臨時程式": [
    "Close temporary app",
    "一時アプリを閉じる",
    "임시 앱 닫기"
  ],
  "關閉設定": [
    "Close settings",
    "設定を閉じる",
    "설정 닫기"
  ],
  "預先分析 EXE 與同資料夾的 DLL，建立 resident AOT 快取，供啟用持久快取時使用。也可以稍後從「程式設定」開始。": [
    "Analyze the EXE and nearby DLLs to build resident AOT caches for use when persistent caching is enabled. You can also start later from App settings.",
    "EXE と同じフォルダーの DLL を解析し、永続キャッシュを有効にした際に使用する resident AOT キャッシュを作成します。後で「アプリ設定」からも開始できます。",
    "EXE와 같은 폴더의 DLL을 분석하여 영구 캐시가 활성화되었을 때 사용할 resident AOT 캐시를 만듭니다. 나중에 앱 설정에서도 시작할 수 있습니다."
  ],
  "預設 256，可設為 16–8192。按使用量配置，於下次執行生效。": [
    "Default: 256. Range: 16–8192. Allocated on demand; applies on the next launch.",
    "既定値は 256、範囲は 16〜8192。使用量に応じて確保され、次回起動時に適用されます。",
    "기본값: 256, 범위: 16~8192. 사용량에 따라 할당되며 다음 실행부터 적용됩니다."
  ],
  "預設安裝到 C 槽的 Program Files": [
    "Defaults to Program Files on C:",
    "既定は C: の Program Files",
    "기본 설치 위치: C:의 Program Files"
  ],
  "預設磁碟映射": [
    "Default drive mappings",
    "既定のドライブ割り当て",
    "기본 드라이브 매핑"
  ],
  "預設：{0}/Program Files": [
    "Default: {0}/Program Files",
    "既定：{0}/Program Files",
    "기본값: {0}/Program Files"
  ],
  "＋ 加入第一個 EXE": [
    "＋ Add your first EXE",
    "＋ 最初の EXE を追加",
    "＋ 첫 EXE 추가"
  ],
  "＋ 新增磁碟映射": [
    "＋ Add drive mapping",
    "＋ ドライブを追加",
    "＋ 드라이브 매핑 추가"
  ]
};
Object.assign(translations, {
  "讀取程式": [
    "Reading app",
    "アプリを読み込み中",
    "앱 읽는 중"
  ],
  "正在讀取程式與 DLL…": [
    "Reading app and DLLs…",
    "アプリと DLL を読み込み中…",
    "앱 및 DLL 읽는 중…"
  ],
  "編譯 resident AOT 區塊": [
    "Compiling resident AOT blocks",
    "resident AOT ブロックをコンパイル中",
    "resident AOT 블록 컴파일 중"
  ],
  "儲存快取": [
    "Saving cache",
    "キャッシュを保存中",
    "캐시 저장 중"
  ],
  "掃描程式與 DLL": [
    "Scanning app and DLLs",
    "アプリと DLL をスキャン中",
    "앱 및 DLL 검색 중"
  ],
  "已分析 {0} / {1} 個檔案": [
    "Analyzed {0} / {1} files",
    "{0} / {1} ファイルを解析済み",
    "파일 {0} / {1}개 분석됨"
  ],
  "正在分析 {0}": [
    "Analyzing {0}",
    "{0} を解析中",
    "{0} 분석 중"
  ],
  "已取消": [
    "Cancelled",
    "キャンセル済み",
    "취소됨"
  ],
  "已取消分析。已完成檔案的快取仍可使用。": [
    "Analysis cancelled. Completed caches remain available.",
    "解析を中止しました。完了済みのキャッシュは引き続き使用できます。",
    "분석이 취소되었습니다. 완료된 캐시는 계속 사용할 수 있습니다."
  ],
  "分析失敗": [
    "Analysis failed",
    "解析失敗",
    "분석 실패"
  ],
  "分析未全部完成": [
    "Analysis did not complete",
    "解析が完了しませんでした",
    "분석이 완료되지 않았습니다"
  ],
  "分析完成，已建立 resident AOT 快取；需啟用持久快取才會載入。": [
    "Analysis complete. Resident AOT caches are ready; enable persistent caching to load them.",
    "解析が完了し、resident AOT キャッシュを作成しました。読み込みには永続キャッシュを有効にする必要があります。",
    "분석이 완료되어 resident AOT 캐시를 만들었습니다. 로드하려면 영구 캐시를 활성화해야 합니다."
  ],
  "正在取消…": [
    "Cancelling…",
    "キャンセル中…",
    "취소 중…"
  ],
  "找不到此程式": [
    "App not found",
    "アプリが見つかりません",
    "앱을 찾을 수 없음"
  ],
  "已有加速分析正在進行": [
    "An analysis is already running.",
    "解析がすでに実行中です。",
    "이미 분석이 실행 중입니다."
  ],
  "請先停止此程式再進行加速分析": [
    "Stop this app before analyzing it.",
    "解析する前にアプリを停止してください。",
    "분석하기 전에 앱을 중지하세요."
  ],
  "請選擇 .exe 檔案": [
    "Choose an .exe file.",
    ".exe ファイルを選択してください。",
    ".exe 파일을 선택하세요."
  ],
  "不是有效的 Windows PE 程式": [
    "Not a valid Windows PE executable.",
    "有効な Windows PE 実行ファイルではありません。",
    "유효한 Windows PE 실행 파일이 아닙니다."
  ],
  "請選擇 Windows EXE，不能使用 DLL": [
    "Choose a Windows EXE, not a DLL.",
    "DLL ではなく Windows EXE を選択してください。",
    "DLL이 아닌 Windows EXE를 선택하세요."
  ],
  "不支援的語言或介面配色": [
    "Unsupported language or color theme.",
    "未対応の言語または配色です。",
    "지원하지 않는 언어 또는 색상 테마입니다."
  ],
  "請指定存在的絕對目錄路徑": [
    "Enter an existing absolute folder path.",
    "存在するフォルダーの絶対パスを入力してください。",
    "존재하는 폴더의 절대 경로를 입력하세요."
  ],
  "附加磁碟代號限 D: 至 Z:": [
    "Additional drive letters must be D: through Z:.",
    "追加ドライブは D:〜Z: を指定してください。",
    "추가 드라이브 문자는 D:부터 Z:까지 사용할 수 있습니다."
  ],
  "臨時執行每次請選擇一個 EXE": [
    "Choose one EXE for a temporary run.",
    "一時実行する EXE を 1 つ選択してください。",
    "임시 실행할 EXE 하나를 선택하세요."
  ],
  "無法開啟檔案選擇器": [
    "Unable to open file picker.",
    "ファイル選択画面を開けません。",
    "파일 선택기를 열 수 없습니다."
  ],
  "已儲存。介面偏好已套用，進階設定於下次執行生效。": [
    "Saved. Appearance applied; advanced settings take effect on the next launch.",
    "保存しました。表示設定は適用済み、詳細設定は次回起動時に反映されます。",
    "저장했습니다. 화면 설정은 적용되었으며 고급 설정은 다음 실행부터 적용됩니다."
  ]
});
const supportedLanguages = ['zh-Hant', 'en', 'ja', 'ko'];
function resolveLanguage(preference, languages = navigator.languages || [navigator.language]) {
  if (supportedLanguages.includes(preference)) return preference;
  for (const language of languages) {
    const base = String(language).toLowerCase().split(/[-_]/)[0];
    if (base === 'zh') return 'zh-Hant';
    if (['en', 'ja', 'ko'].includes(base)) return base;
  }
  return 'en';
}
let currentLanguage = resolveLanguage('auto');
function t(source, ...values) {
  const key = Array.isArray(source) ? source.reduce((text, part, index) => text + (index ? `{${index - 1}}` : '') + part, '') : source;
  const index = ['en', 'ja', 'ko'].indexOf(currentLanguage);
  const translated = index < 0 ? key : translations[key]?.[index] ?? key;
  return values.length ? translated.replace(/\{(\d+)\}/g, (match, index) => index < values.length ? String(values[index]) : match) : translated;
}
// 只記錄啟動器原有的靜態文案。動態文案透過 t()，使用者資料保持原樣。
const localizedNodes = [], localizedAttributes = [];
const textWalker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
while (textWalker.nextNode()) {
  const node = textWalker.currentNode;
  if (node.parentElement.closest('script, style')) continue;
  const key = node.textContent.trim();
  if (Object.hasOwn(translations, key)) localizedNodes.push({ node, key, leading: node.textContent.match(/^\s*/)[0], trailing: node.textContent.match(/\s*$/)[0] });
}
for (const node of document.querySelectorAll('[title], [placeholder], [aria-label]')) {
  for (const attribute of ['title', 'placeholder', 'aria-label']) {
    const key = node.getAttribute(attribute);
    if (Object.hasOwn(translations, key)) localizedAttributes.push({ node, attribute, key });
  }
}
let appearanceSignature = '';
function applyAppearance(language = 'auto', theme = 'forest') {
  currentLanguage = resolveLanguage(language);
  const signature = currentLanguage + ':' + theme;
  if (appearanceSignature === signature) return;
  appearanceSignature = signature;
  document.documentElement.lang = currentLanguage;
  document.documentElement.dataset.theme = ['forest', 'ocean', 'violet', 'amber', 'dark'].includes(theme) ? theme : 'forest';
  for (const { node, key, leading, trailing } of localizedNodes) if (node.isConnected) node.textContent = leading + t(key) + trailing;
  for (const { node, attribute, key } of localizedAttributes) if (node.isConnected) node.setAttribute(attribute, t(key));
}
applyAppearance();

// 只格式化啟動器已知的進度訊息；診斷與客體輸出保持原文。
function translateMessage(message) {
 if (!message) return message;
 let match = message.match(/^已分析 (\d+) \/ (\d+) 個檔案$/);
 if (match) return t`已分析 ${match[1]} / ${match[2]} 個檔案`;
 match = message.match(/^正在分析 (.+)$/);
 if (match) return t`正在分析 ${match[1]}`;
 return t(message);
}
