/* AC3Forge docs: two small, independent, page-scoped enhancements.
   Neither touches page content beyond adding a filter box or a small
   inline marker — the caveats already written on these pages stay as
   written; nothing here summarises or overrides them. */
(function () {
  "use strict";

  function onReady(fn) {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", fn);
    } else {
      fn();
    }
  }

  // ---- Capabilities page: filter the reference tables ----
  function initCapabilitiesFilter() {
    if (!/\/library\/capabilities\/?$/.test(location.pathname)) return;
    var content = document.querySelector(".md-content__inner");
    if (!content) return;
    var tables = content.querySelectorAll("table");
    if (!tables.length) return;

    var bar = document.createElement("div");
    bar.className = "ac3f-cap-filter";
    var input = document.createElement("input");
    input.type = "text";
    input.placeholder = 'Filter every table on this page — try "atmos" or "vbr"…';
    var count = document.createElement("span");
    count.className = "ac3f-cap-filter-count";
    bar.appendChild(input);
    bar.appendChild(count);
    tables[0].parentNode.insertBefore(bar, tables[0]);

    input.addEventListener("input", function () {
      var q = input.value.trim().toLowerCase();
      var shown = 0, total = 0;
      tables.forEach(function (table) {
        table.querySelectorAll("tr").forEach(function (row) {
          if (row.querySelector("th")) return;
          total++;
          var match = !q || row.textContent.toLowerCase().indexOf(q) !== -1;
          row.style.display = match ? "" : "none";
          if (match) shown++;
        });
      });
      count.textContent = q ? shown + " of " + total : "";
    });
  }

  // ---- Forge page: mark the install method for the visitor's OS ----
  // OS-level only, on purpose — this can't know architecture, and doesn't
  // pretend to. Adds a badge next to the matching list item; never hides
  // or rewrites the other options or their caveats.
  function detectOS() {
    try {
      var plat = navigator.platform || "", ua = navigator.userAgent || "";
      if (/Win/i.test(plat)) return "windows";
      if (/Mac/i.test(plat)) return "mac";
      if (/Linux/i.test(plat) && !/Android/i.test(ua)) return "linux";
    } catch (e) {}
    return null;
  }

  function initPlatformMarker() {
    if (!/\/forge\/?$/.test(location.pathname)) return;
    var os = detectOS();
    if (!os) return;
    var needle = os === "windows" ? "winget" : "homebrew";
    var items = document.querySelectorAll(".md-content__inner li");
    for (var i = 0; i < items.length; i++) {
      if (items[i].textContent.toLowerCase().indexOf(needle) !== -1) {
        var badge = document.createElement("span");
        badge.className = "ac3f-platform-marker";
        badge.textContent = "this is you";
        items[i].insertBefore(badge, items[i].firstChild.nextSibling);
        break;
      }
    }
  }

  onReady(function () {
    initCapabilitiesFilter();
    initPlatformMarker();
  });
})();
