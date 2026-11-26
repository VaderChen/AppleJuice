const homeCopy = {
  gameplay: {"zh": "實機遊玩", "ja": "プレイ映像", "ko": "플레이 영상"},
  tested: {"zh": "相容性清單", "ja": "動作確認済みゲーム", "ko": "테스트한 게임"},
  benchmarks: {"zh": "效能測試", "ja": "ベンチマーク", "ko": "벤치마크"},
  headline: {"zh": "在 Apple Silicon 上玩 Windows 遊戲", "ja": "Apple Silicon で Windows ゲームを。", "ko": "Apple Silicon에서 즐기는 Windows 게임"},
  intro: {"zh": "在 Mac 上遊玩支援的 Windows 遊戲。AppleJuice 將 x86 與 x64 程式碼轉譯為 ARM64，並透過 Metal 呈現 Direct3D 畫面。", "ja": "対応する Windows ゲームを Mac でプレイ。AppleJuice は x86・x64 コードを ARM64 に変換し、Direct3D の描画に Metal を使います。", "ko": "지원되는 Windows 게임을 Mac에서 플레이하세요. AppleJuice는 x86 및 x64 코드를 ARM64로 변환하고 Metal을 통해 Direct3D 그래픽을 표시합니다."},
  download: {"zh": "下載 Mac 版", "ja": "Mac 版をダウンロード", "ko": "Mac용 다운로드"},
  findGame: {"zh": "查詢遊戲 →", "ja": "ゲームを検索 →", "ko": "게임 찾기 →"},
  requirements: {"zh": "Apple Silicon · macOS 13 或以上版本", "ja": "Apple Silicon · macOS 13 以降", "ko": "Apple Silicon · macOS 13 이상"},
  showcase: {"zh": "實機遊玩展示", "ja": "実機プレイ映像", "ko": "실제 플레이 영상"},
  capture: {"zh": "兩段遊玩畫面均使用 Apple M4 Pro、64 GiB 統一記憶體、macOS 27.2.1 及 AppleJuice 0.26.1126（build 727）錄製。兩款遊戲皆以 Direct3D 11 輸出 1920 × 1080 畫面，Launcher 的 MetalFX 放大與畫面內插均已關閉。", "ja": "両作品とも Apple M4 Pro、ユニファイドメモリ 64 GiB、macOS 27.2.1、AppleJuice 0.26.1126（ビルド 727）で収録しました。Direct3D 11 で 1920 × 1080 を出力し、ランチャーの MetalFX 拡大とフレーム補間はオフにしています。", "ko": "두 게임 모두 Apple M4 Pro, 통합 메모리 64 GiB, macOS 27.2.1, AppleJuice 0.26.1126(빌드 727)에서 녹화했습니다. Direct3D 11로 1920 × 1080을 출력하며 런처의 MetalFX 업스케일링과 프레임 보간은 껐습니다."},
  videoNote: {"zh": "0:00 從 Steam 收藏庫啟動《邊境獵人》。全片為即時視窗錄影，無聲音，以 30 fps 編碼。", "ja": "0:00 に Steam ライブラリから『フロンティアハンター』を起動します。ウインドウを実時間で収録した無音の映像で、30 fps でエンコードしています。", "ko": "0:00에 Steam 라이브러리에서 프런티어 헌터를 실행합니다. 창을 실시간으로 녹화한 무음 영상으로, 30 fps로 인코딩했습니다."},
  openVideo: {"zh": "開啟 MP4", "ja": "MP4 を開く", "ko": "MP4 열기"},
  frontier: {"zh": "0:12 · 邊境獵人：艾爾莎的命運之輪", "ja": "0:12 · フロンティアハンター ～エルザの運命の輪～", "ko": "0:12 · 프런티어 헌터: 엘사의 운명의 바퀴"},
  frontierBuild: {"zh": "Steam 版本 15701452 · 2 個 CPU 核心", "ja": "Steam ビルド 15701452 · CPU 2 コア", "ko": "Steam 빌드 15701452 · CPU 2코어"},
  tales: {"zh": "0:44 · 破曉傳奇", "ja": "0:44 · テイルズ オブ アライズ", "ko": "0:44 · 테일즈 오브 어라이즈"},
  talesBuild: {"zh": "Steam 版本 12823607 · 4 個 CPU 核心", "ja": "Steam ビルド 12823607 · CPU 4 コア", "ko": "Steam 빌드 12823607 · CPU 4코어"},
  results: {"zh": "遊戲實測摘錄", "ja": "ゲームの動作確認結果", "ko": "게임 테스트 결과"},
  examples: {"zh": "《三國志 11 威力加強版》在 build 727 的預設設定下達到遊戲幀率上限。《死亡擱淺 導演剪輯版》可載入第三章存檔，但因白色閃爍與效能不穩，評級為「可開啟」。", "ja": "『三國志11 パワーアップキット』はビルド 727 の標準設定でゲームのフレームレート上限に達します。『DEATH STRANDING DIRECTOR’S CUT』は第3章のセーブを読み込めますが、白いちらつきと速度のばらつきがあり、評価は「起動可能」です。", "ko": "삼국지 11 파워업키트는 빌드 727의 기본 설정에서 게임의 프레임 제한에 도달합니다. 데스 스트랜딩 디렉터스 컷은 3장 저장 파일을 불러올 수 있지만 흰색 깜박임과 불안정한 성능으로 ‘실행 가능’ 등급입니다."},
  records: {"zh": "查看遊戲與工具紀錄", "ja": "ゲームとツールの記録を見る", "ko": "게임 및 도구 기록 보기"},
  runtime: {"zh": "執行環境", "ja": "ランタイムの仕組み", "ko": "런타임 구조"},
  runtimeInfo: {"zh": "常駐轉譯器產生 ARM64 程式碼。Windows 服務銜接遊戲與原生繪圖、輸入及音訊；輸出階段可選用 MetalFX 空間放大與耳機混音。", "ja": "常駐トランスレータが ARM64 コードを生成し、Windows サービスがゲームをネイティブの描画・入力・音声に接続します。出力時には MetalFX 空間アップスケーリングとヘッドホンミキシングを選べます。", "ko": "상주 변환기가 ARM64 코드를 생성하고 Windows 서비스가 게임을 네이티브 그래픽, 입력 및 오디오에 연결합니다. 출력 단계에서 MetalFX 공간 업스케일링과 헤드폰 믹싱을 선택할 수 있습니다."},
  architecture: {"zh": "查看架構說明", "ja": "アーキテクチャを読む", "ko": "아키텍처 보기"},
  performance: {"zh": "CPU 效能", "ja": "CPU 性能", "ko": "CPU 성능"},
  measurements: {"zh": "比較 Apple M4 Pro 在 Geekbench 7 各項測試的分數。", "ja": "Apple M4 Pro の Geekbench 7 スコアを項目別に比較できます。", "ko": "Apple M4 Pro의 Geekbench 7 점수를 항목별로 비교하세요."},
  readScores: {"zh": "查看效能分數", "ja": "スコアを見る", "ko": "점수 보기"},
  gettingStarted: {"zh": "開始使用", "ja": "はじめに", "ko": "시작하기"},
  installGame: {"zh": "如何安裝遊戲？", "ja": "ゲームのインストール方法は？", "ko": "게임은 어떻게 설치하나요?"},
  installSteps: {"zh": "將發行版 DMG 裡的 AppleJuice.app 複製到「應用程式」。開啟 Launcher，按「加入 EXE」選擇遊戲，確認磁碟映射後按下執行。", "ja": "配布 DMG から AppleJuice.app を「アプリケーション」にコピーします。ランチャーの「EXE を追加」でゲームを選び、ドライブの割り当てを確認して実行します。", "ko": "배포 DMG에서 AppleJuice.app을 응용 프로그램 폴더로 복사하세요. 런처에서 EXE 추가를 눌러 게임을 선택하고 드라이브 매핑을 확인한 뒤 실행하세요."},
  installSteam: {"zh": "如何安裝 Steam？", "ja": "Steam のインストール方法は？", "ko": "Steam은 어떻게 설치하나요?"},
  steamSteps: {"zh": "按「臨時執行」，在 macOS 選檔視窗選取 SteamSetup.exe。安裝完成後，Steam.exe 會出現在 Launcher 左側欄。", "ja": "「一時実行」を選び、macOS のファイル選択画面で SteamSetup.exe を開きます。インストールが完了すると、ランチャーのサイドバーに Steam.exe が表示されます。", "ko": "‘임시 실행’을 누르고 macOS 파일 선택 창에서 SteamSetup.exe를 선택하세요. 설치가 끝나면 런처 왼쪽 목록에 Steam.exe가 나타납니다."},
  watchInstall: {"zh": "觀看 Steam 安裝實錄（39 秒）", "ja": "Steam のインストール映像を見る（39 秒）", "ko": "Steam 설치 영상 보기(39초)"},
  whichGames: {"zh": "哪些遊戲可以玩？", "ja": "どのゲームが動作しますか？", "ko": "어떤 게임을 플레이할 수 있나요?"},
  ratings: {"zh": "實測清單分別列出預設設定下運作良好、透過 MetalFX 改善、執行緩慢、僅能開啟及無法執行的結果。請核對遊戲版本與設定。", "ja": "動作確認リストは標準設定で快適、MetalFX で改善、低速、起動のみ、起動不可の結果に分かれます。ゲームの版と設定をご確認ください。", "ko": "테스트 목록은 기본 설정에서 원활한 게임, MetalFX로 개선되는 게임, 느린 게임, 실행만 가능한 게임과 실행에 실패한 게임을 구분합니다. 게임 버전과 설정을 확인하세요."},
  report: {"zh": "提供測試結果或回報問題", "ja": "テスト結果を共有・問題を報告", "ko": "테스트 결과 공유 또는 문제 신고"},
  language: {"zh": "語言", "ja": "言語", "ko": "언어"},
  auto: {"zh": "自動偵測", "ja": "自動判定", "ko": "자동 감지"},
  main: {"zh": "主導覽", "ja": "メインナビゲーション", "ko": "주 탐색"},
  guides: {"zh": "產品指南", "ja": "製品ガイド", "ko": "제품 안내"},
  movie: {"zh": "78 秒《邊境獵人》與《破曉傳奇》實機錄影", "ja": "フロンティアハンターとテイルズ オブ アライズの実機映像（78 秒）", "ko": "프런티어 헌터와 테일즈 오브 어라이즈 실기 영상(78초)"},
  cabin: {"zh": "播放《邊境獵人》：艾爾莎在艙房中跑動並折返。", "ja": "フロンティアハンターを再生：エルザが船室を走って折り返す。", "ko": "프런티어 헌터 재생: 엘사가 선실을 가로질러 달리다 방향을 바꿉니다."},
  battle: {"zh": "播放《破曉傳奇》：隊伍在地下城遇敵並進入戰鬥。", "ja": "テイルズ オブ アライズを再生：一行がダンジョンで敵に遭遇し、戦闘に入る。", "ko": "테일즈 오브 어라이즈 재생: 일행이 던전에서 적을 만나 전투에 들어갑니다."},
};

const { current, requested } = siteLanguage;
document.documentElement.lang = current === 'zh' ? 'zh-Hant' : current;
if (current !== 'en') {
  for (const node of document.querySelectorAll('[data-copy]')) {
    node.textContent = homeCopy[node.dataset.copy][current];
  }
  for (const node of document.querySelectorAll('[data-label]')) {
    node.setAttribute('aria-label', homeCopy[node.dataset.label][current]);
  }
  for (const image of document.querySelectorAll('[data-alt]')) {
    image.alt = homeCopy[image.dataset.alt][current];
  }
  document.title = 'AppleJuice | ' + homeCopy.headline[current];
  document.querySelector('meta[name="description"]').content = homeCopy.intro[current];
}
const languageMenu = document.getElementById('language');
languageMenu.value = requested || '';
document.getElementById('language-control').hidden = false;
languageMenu.addEventListener('change', () => {
  const url = new URL(location.href);
  if (languageMenu.value) url.searchParams.set('lang', languageMenu.value);
  else url.searchParams.delete('lang');
  location.assign(url);
});
if (requested) {
  for (const link of document.querySelectorAll('a[href="tested/"], a[href="geekbench7/"]')) {
    link.setAttribute('href', link.getAttribute('href') + '?lang=' + requested);
  }
}
