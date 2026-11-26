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
