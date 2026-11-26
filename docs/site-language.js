// 各頁沿用相同的網址代碼，中文內容採繁體。
const siteLanguage = (() => {
  const supported = new Set(['en', 'zh', 'ja', 'ko']);
  function match(tag) {
    try {
      const code = new Intl.Locale(tag).language;
      return supported.has(code) ? code : null;
    } catch {
      return null;
    }
  }
  const requested = match(new URLSearchParams(location.search).get('lang'));
  const detected = (navigator.languages || [navigator.language]).map(match).find(Boolean);
  return { requested, current: requested || detected || 'en' };
})();

const navigationCopy = {
  main: {"zh": "主導覽", "ja": "メインナビゲーション", "ko": "주 탐색"},
  gameplay: {"zh": "實機遊玩", "ja": "プレイ映像", "ko": "플레이 영상"},
  tested: {"zh": "相容性清單", "ja": "動作確認済みゲーム", "ko": "테스트한 게임"},
  benchmarks: {"zh": "效能測試", "ja": "ベンチマーク", "ko": "벤치마크"},
  language: {"zh": "語言", "ja": "言語", "ko": "언어"},
  auto: {"zh": "自動偵測", "ja": "自動判定", "ko": "자동 감지"},
  guides: {"zh": "產品指南", "ja": "製品ガイド", "ko": "제품 안내"},
};
if (siteLanguage.current !== 'en') {
  for (const node of document.querySelectorAll('[data-nav-copy]')) {
    node.textContent = navigationCopy[node.dataset.navCopy][siteLanguage.current];
  }
  for (const node of document.querySelectorAll('[data-nav-label]')) {
    node.setAttribute('aria-label', navigationCopy[node.dataset.navLabel][siteLanguage.current]);
  }
}
const languageMenu = document.getElementById('language');
languageMenu.value = siteLanguage.requested || '';
document.getElementById('language-control').hidden = false;
languageMenu.addEventListener('change', () => {
  const url = new URL(location.href);
  if (languageMenu.value) url.searchParams.set('lang', languageMenu.value);
  else url.searchParams.delete('lang');
  location.assign(url);
});
if (siteLanguage.requested) {
  for (const link of document.querySelectorAll('.site-nav a, main a[href="architecture/"], main a[href="tested/"], main a[href="geekbench7/"]')) {
    const url = new URL(link.href);
    if (url.origin === location.origin) {
      url.searchParams.set('lang', siteLanguage.requested);
      link.href = url.href;
    }
  }
}
