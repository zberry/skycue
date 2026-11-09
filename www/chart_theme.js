// skycue web UI: the ECharts theme of the Stats view, built from the page's
// colour tokens (style.css :root) so the charts match the map and panels.
// window.skycueChartTheme() returns the theme object; it is registered once
// as 'skycue' when ECharts is loaded (echarts.init(el, 'skycue')). Beside
// the ECharts keys the object carries, for the view's own series:
//   decision: {sent, allow, range, elev, blocked, unknown}  (the decision colours)
//   bands: [8 colours]  the altitude bands of /api/stats/coverage, in its
//          order: 0-2.5k, 2.5-5k, 5-10k, 10-20k, 20-30k, 30-40k, 40k+, unknown
// (warm low to cool high, grey for unknown; see www/README.md).
(function () {
  'use strict';

  var BANDS = ['#ff7b72', '#f0883e', '#e3b341', '#7ee787', '#56d4dd', '#58a6ff', '#d2a8ff', '#6e7681'];

  function token(style, name, fallback) {
    var v = style ? style.getPropertyValue(name).trim() : '';
    return v || fallback;
  }

  function skycueChartTheme() {
    var cs = typeof getComputedStyle === 'function' ? getComputedStyle(document.documentElement) : null;
    var c = {
      bg: token(cs, '--bg', '#0d1117'), panel: token(cs, '--panel', '#161b22'), panel2: token(cs, '--panel-2', '#1c232c'),
      line: token(cs, '--line', '#30363d'), text: token(cs, '--text', '#e6edf3'), muted: token(cs, '--muted', '#8b949e'),
      accent: token(cs, '--accent', '#58a6ff'), sent: token(cs, '--sent', '#3fb950'), allow: token(cs, '--allow', '#58a6ff'),
      range: token(cs, '--range', '#8b949e'), elev: token(cs, '--elev', '#f0883e'), blocked: token(cs, '--blocked', '#f85149'),
      unknown: token(cs, '--unknown', '#d2a8ff'), uat: token(cs, '--uat', '#d2a8ff')
    };
    var axis = function () {
      return {
        axisLine: { show: true, lineStyle: { color: c.line } },
        axisTick: { show: true, lineStyle: { color: c.line } },
        axisLabel: { show: true, color: c.muted },
        splitLine: { show: true, lineStyle: { color: [c.line], opacity: 0.5 } },
        splitArea: { show: false },
        nameTextStyle: { color: c.muted }
      };
    };
    var cat = axis();
    cat.splitLine = { show: false, lineStyle: { color: [c.line] } };
    return {
      // Series colours when a chart names none: accent first, then
      // distinct hues that read on the dark panel.
      color: [c.accent, c.sent, c.elev, c.unknown, c.blocked, '#e3b341', '#56d4dd', c.range],
      backgroundColor: 'transparent',
      textStyle: { color: c.text, fontFamily: 'inherit' },
      title: { textStyle: { color: c.text }, subtextStyle: { color: c.muted } },
      legend: { textStyle: { color: c.muted }, inactiveColor: c.line, pageTextStyle: { color: c.muted } },
      tooltip: {
        backgroundColor: c.panel2, borderColor: c.line, borderWidth: 1,
        textStyle: { color: c.text, fontSize: 12 },
        axisPointer: { lineStyle: { color: c.muted }, crossStyle: { color: c.muted } }
      },
      categoryAxis: cat,
      valueAxis: axis(),
      logAxis: axis(),
      timeAxis: axis(),
      angleAxis: axis(),
      radiusAxis: axis(),
      polar: {},
      grid: { borderColor: c.line },
      line: { symbol: 'none', smooth: false, lineStyle: { width: 1.5 } },
      bar: { itemStyle: { barBorderWidth: 0 } },
      pie: { itemStyle: { borderColor: c.panel, borderWidth: 1 } },
      heatmap: {},
      dataZoom: {
        borderColor: c.line, textStyle: { color: c.muted }, fillerColor: 'rgba(88,166,255,0.15)',
        handleStyle: { color: c.panel2, borderColor: c.muted },
        dataBackground: { lineStyle: { color: c.muted }, areaStyle: { color: c.line } },
        moveHandleStyle: { color: c.muted }
      },
      visualMap: { textStyle: { color: c.muted } },
      markLine: { lineStyle: { color: c.muted } },
      decision: { sent: c.sent, allow: c.allow, range: c.range, elev: c.elev, blocked: c.blocked, unknown: c.unknown },
      bands: BANDS.slice(),
      tokens: c
    };
  }
  skycueChartTheme.BANDS = BANDS;
  window.skycueChartTheme = skycueChartTheme;

  // Registered once; the view's own keys (decision, bands, tokens) stay out
  // of the registered copy, ECharts has no use for them.
  if (window.echarts && typeof window.echarts.registerTheme === 'function') {
    var theme = skycueChartTheme();
    delete theme.decision;
    delete theme.bands;
    delete theme.tokens;
    window.echarts.registerTheme('skycue', theme);
  }
})();
