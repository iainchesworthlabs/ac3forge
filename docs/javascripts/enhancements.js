/* AC3Forge docs: small, independent, page-scoped enhancements. */
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

  // ---- Home page: product/platform/variant download selector ----
  function uniqueValues(entries, field) {
    var seen = Object.create(null);
    return entries.reduce(function (values, entry) {
      var value = entry[field];
      if (!seen[value]) {
        seen[value] = true;
        values.push(value);
      }
      return values;
    }, []);
  }

  function resetSelect(select, prompt, values) {
    select.replaceChildren();
    var promptOption = document.createElement("option");
    promptOption.value = "";
    promptOption.textContent = prompt;
    select.appendChild(promptOption);
    values.forEach(function (value) {
      var option = document.createElement("option");
      option.value = value;
      option.textContent = value;
      select.appendChild(option);
    });
    select.disabled = values.length === 0;
  }

  function renderDownloadResult(container, entry, catalogue) {
    container.replaceChildren();

    var status = document.createElement("p");
    status.className = "ac3f-selector-status ac3f-selector-status-" + entry.state;
    status.textContent = catalogue.states[entry.state].label;
    status.title = catalogue.states[entry.state].description;

    var title = document.createElement("h3");
    title.textContent = entry.artifact;

    var note = document.createElement("p");
    note.textContent = entry.note;

    var action = document.createElement("a");
    action.className = "ac3f-btn ac3f-btn-primary";
    action.href = new URL(entry.url, window.location.href).href;
    action.textContent = entry.action;

    container.appendChild(status);
    container.appendChild(title);
    container.appendChild(note);
    container.appendChild(action);
  }

  function initDownloadSelector() {
    var selector = document.getElementById("ac3f-download-selector");
    if (!selector) return;

    var product = selector.querySelector('[data-selector="product"]');
    var platform = selector.querySelector('[data-selector="platform"]');
    var variant = selector.querySelector('[data-selector="variant"]');
    var result = document.getElementById("ac3f-download-result");
    if (!product || !platform || !variant || !result) return;

    fetch(selector.dataset.catalogueUrl, { credentials: "same-origin" })
      .then(function (response) {
        if (!response.ok) throw new Error("HTTP " + response.status);
        return response.json();
      })
      .then(function (catalogue) {
        var entries = catalogue.downloads;
        resetSelect(product, "Choose a product", uniqueValues(entries, "product"));

        product.addEventListener("change", function () {
          var matches = entries.filter(function (entry) {
            return entry.product === product.value;
          });
          resetSelect(platform, "Choose a platform", uniqueValues(matches, "platform"));
          resetSelect(variant, "Choose a variant", []);
          result.innerHTML =
            "<p>Choose a platform and variant to see the available path.</p>";
        });

        platform.addEventListener("change", function () {
          var matches = entries.filter(function (entry) {
            return (
              entry.product === product.value && entry.platform === platform.value
            );
          });
          resetSelect(variant, "Choose a variant", uniqueValues(matches, "variant"));
          result.innerHTML = "<p>Choose a variant to see the available path.</p>";
        });

        variant.addEventListener("change", function () {
          var match = entries.find(function (entry) {
            return (
              entry.product === product.value &&
              entry.platform === platform.value &&
              entry.variant === variant.value
            );
          });
          if (match) renderDownloadResult(result, match, catalogue);
        });
      })
      .catch(function () {
        result.replaceChildren();
        var message = document.createElement("p");
        message.textContent = "The download catalogue could not be loaded.";
        var link = document.createElement("a");
        link.href = new URL("platforms/", window.location.href).href;
        link.textContent = "Open the platform support matrix";
        result.appendChild(message);
        result.appendChild(link);
      });
  }

  onReady(function () {
    initCapabilitiesFilter();
    initPlatformMarker();
    initDownloadSelector();
  });
})();
