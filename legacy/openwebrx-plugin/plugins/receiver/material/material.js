(function () {
  "use strict";

  var ICONS = {
    grid: '<rect width="18" height="18" x="3" y="3" rx="2"/><path d="M3 9h18"/><path d="M9 21V9"/>',
    close: '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
    external: '<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>',
    settings: '<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>',
    radio: '<path d="M4.9 19.1C1 15.2 1 8.8 4.9 4.9"/><path d="M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.5"/><circle cx="12" cy="12" r="2"/><path d="M16.2 7.8c2.3 2.3 2.3 6.1 0 8.5"/><path d="M19.1 4.9C23 8.8 23 15.2 19.1 19.1"/>',
    broadcast: '<circle cx="12" cy="12" r="2"/><path d="M4.93 4.93a10 10 0 0 0 0 14.14"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14"/><path d="M7.76 7.76a6 6 0 0 0 0 8.49"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49"/>',
    plane: '<path d="M17.8 19.2 16 11l3.5-3.5C21 6 21.5 4 21 3c-1-.5-3 0-4.5 1.5L13 8 4.8 6.2c-.5-.1-.9.1-1.1.5l-.3.5c-.2.5-.1 1 .3 1.3L9 12l-2 3H4l-1 1 3 2 2 3 1-1v-3l3-2 3.5 5.3c.3.4.8.5 1.3.3l.5-.2c.5-.3.7-.7.6-1.2z"/>',
    message: '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>',
    ship: '<path d="M2 21c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1s1.2 1 2.5 1c2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1"/><path d="M19.38 20A11.6 11.6 0 0 0 21 14l-9-4-9 4c0 2.9.94 5.34 2.81 7.76"/><path d="M19 13V7a2 2 0 0 0-2-2H7a2 2 0 0 0-2 2v6"/><path d="M12 10v4"/><path d="M12 2v3"/>',
    satellite: '<path d="M13 7 9 3 5 7l4 4"/><path d="m17 11 4 4-4 4-4-4"/><path d="m8 12 4 4 6-6-4-4Z"/><path d="m16 8 3-3"/><path d="M9 21a6 6 0 0 0-6-6"/>',
    antenna: '<path d="M2 20h.01"/><path d="M7 20v-4"/><path d="M12 20v-8"/><path d="M17 20V8"/><path d="M22 4v16"/>',
    cpu: '<rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><path d="M15 2v2"/><path d="M15 20v2"/><path d="M2 15h2"/><path d="M2 9h2"/><path d="M20 15h2"/><path d="M20 9h2"/><path d="M9 2v2"/><path d="M9 20v2"/>',
    tv: '<rect width="20" height="15" x="2" y="7" rx="2" ry="2"/><polyline points="17 2 12 7 7 2"/>',
    wifi: '<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.859a10 10 0 0 1 14 0"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/>',
    smartphone: '<rect width="14" height="20" x="5" y="2" rx="2" ry="2"/><path d="M12 18h.01"/>',
    zap: '<polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/>'
  };

  var GROUPS = [
    {
      name: "Radiodifusión",
      accent: "#D0BCFF",
      items: [
        { id: "bladerf|fm_broadcast", title: "FM Broadcast", sub: "88 – 108 MHz", badge: "WFM + RDS", icon: "radio" },
        { id: "bladerf|dab", title: "DAB / DAB+", sub: "174 – 240 MHz", badge: "DAB", icon: "broadcast" }
      ]
    },
    {
      name: "Aviación",
      accent: "#8ECDF7",
      items: [
        { id: "bladerf|airband", title: "Airband", sub: "118 – 137 MHz · voz", badge: "AM", icon: "plane" },
        { id: "bladerf|adsb", title: "ADS-B", sub: "1090 MHz · tráfico aéreo", badge: "1090ES", icon: "plane" },
        { id: "bladerf|acars", title: "ACARS", sub: "131.550 MHz", badge: "AM", icon: "message" },
        { id: "bladerf|vdl2", title: "VDL2", sub: "136.975 MHz", badge: "VDL2", icon: "plane" }
      ]
    },
    {
      name: "Marítimo",
      accent: "#7ED6C4",
      items: [
        { id: "bladerf|ais", title: "AIS", sub: "161.975 / 162.025 MHz", badge: "AIS", icon: "ship" }
      ]
    },
    {
      name: "Datos, ISM y telemetría",
      accent: "#F7C873",
      items: [
        { id: "bladerf|pager", title: "POCSAG / FLEX", sub: "466.075 MHz", badge: "PAGER", icon: "message" },
        { id: "bladerf|ism433", title: "ISM 433", sub: "433.920 MHz", badge: "OOK", icon: "antenna" },
        { id: "bladerf|ism868", title: "ISM 868", sub: "868.300 MHz", badge: "OOK", icon: "antenna" },
        { id: "bladerf|lora", title: "LoRa / LoRaWAN", sub: "868.100 MHz", badge: "LoRa", icon: "cpu" },
        { id: "bladerf|meshtastic", title: "Meshtastic", sub: "869.525 MHz", badge: "LoRa", icon: "message" },
        { id: "bladerf|sonde", title: "Radiosonda", sub: "403.000 MHz", badge: "RS41", icon: "satellite" }
      ]
    },
    {
      name: "Radioaficionado",
      accent: "#F2A7C3",
      items: [
        { id: "bladerf|ham2m", title: "2 m", sub: "145.500 MHz", badge: "NFM", icon: "radio" },
        { id: "bladerf|ham70cm", title: "70 cm", sub: "433.500 MHz", badge: "NFM", icon: "radio" }
      ]
    },
    {
      name: "Análisis de espectro",
      accent: "#A6E3A1",
      items: [
        { id: "bladerf|dvbt", title: "DVB-T", sub: "474 MHz · 8 Msps", badge: "ESPECTRO", icon: "tv", spectrum: true },
        { id: "bladerf|gsm900", title: "GSM 900", sub: "942.5 MHz · 20 Msps", badge: "ESPECTRO", icon: "smartphone", spectrum: true },
        { id: "bladerf|lte1800", title: "LTE 1800", sub: "1842.5 MHz · 20 Msps", badge: "ESPECTRO", icon: "smartphone", spectrum: true },
        { id: "bladerf|wifi24", title: "WiFi 2.4 GHz", sub: "2437 MHz · 20 Msps", badge: "ESPECTRO", icon: "wifi", spectrum: true },
        { id: "bladerf|nr3500", title: "5G 3.5 GHz", sub: "3500 MHz · 20 Msps", badge: "ESPECTRO", icon: "zap", spectrum: true }
      ]
    }
  ];

  var titles = {};

  function svg(name, className) {
    return '<svg class="' + (className || "m3-i") + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + (ICONS[name] || ICONS.radio) + "</svg>";
  }

  function el(tag, className, html) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (html !== undefined) node.innerHTML = html;
    return node;
  }

  function build(root) {
    var groups = el("div", "m3-groups");
    groups.id = "m3-groups";

    GROUPS.forEach(function (group) {
      var section = el("section", "m3-group");
      section.style.setProperty("--accent", group.accent);
      section.appendChild(el("div", "m3-group-title", '<span class="m3-group-dot"></span><span>' + group.name + "</span>"));
      var grid = el("div", "m3-grid");
      group.items.forEach(function (item) {
        titles[item.id] = item.title;
        var card = el("button", "m3-card");
        card.type = "button";
        card.setAttribute("data-profile", item.id);
        card.setAttribute("data-title", item.title);
        card.title = item.title + " · " + item.sub;
        card.innerHTML =
          '<span class="m3-card-icon">' + svg(item.icon) + "</span>" +
          '<span class="m3-card-text"><strong>' + item.title + "</strong><small>" + item.sub + "</small></span>" +
          '<span class="m3-card-badge' + (item.spectrum ? " m3-badge-muted" : "") + '">' + item.badge + "</span>";
        card.addEventListener("click", function () {
          selectProfile(item.id, item.title);
        });
        grid.appendChild(card);
      });
      section.appendChild(grid);
      groups.appendChild(section);
    });

    root.innerHTML =
      '<button type="button" class="m3-fab" id="m3-fab" aria-label="Abrir protocolos" aria-expanded="false" title="Protocolos">' +
      svg("grid", "m3-fab-icon") +
      '<span class="m3-fab-label">Protocolos</span>' +
      "</button>" +
      '<div class="m3-scrim" id="m3-scrim"></div>' +
      '<section class="m3-sheet" id="m3-sheet" role="dialog" aria-modal="true" aria-labelledby="m3-sheet-title">' +
      '<header class="m3-sheet-header">' +
      '<div class="m3-brand"><div class="m3-logo">' + svg("broadcast", "m3-logo-icon") + "</div>" +
      '<div><h2 id="m3-sheet-title">Protocolos</h2><p id="m3-device">BladeRF 2.0 micro · RX</p></div></div>' +
      '<div class="m3-header-actions">' +
      '<a class="m3-icon-btn" href="/settings" target="_blank" rel="noopener" title="Ajustes del receptor" aria-label="Ajustes">' + svg("settings") + "</a>" +
      '<button type="button" class="m3-icon-btn" id="m3-close" title="Cerrar" aria-label="Cerrar">' + svg("close") + "</button>" +
      "</div></header>" +
      '<div class="m3-current"><span class="m3-status-dot"></span><span>Perfil actual</span><strong id="m3-current-profile">—</strong>' +
      '<span class="m3-spacer"></span><span class="m3-pill" id="m3-profile-count">—</span></div>' +
      '<div class="m3-sheet-body"></div>' +
      '<footer class="m3-sheet-footer"><span>bladeRF 2.0 micro · OpenWebRX+</span>' +
      '<a class="m3-text-btn" href="/" target="_blank" rel="noopener">Abrir receptor completo ' + svg("external") + "</a>" +
      "</footer></section>" +
      '<div class="m3-toast" id="m3-toast" role="status" aria-live="polite"></div>';

    root.querySelector(".m3-sheet-body").appendChild(groups);

    var fab = root.querySelector("#m3-fab");
    var scrim = root.querySelector("#m3-scrim");
    var close = root.querySelector("#m3-close");

    fab.addEventListener("click", function () {
      if (root.classList.contains("m3-open")) closeSheet();
      else openSheet();
    });
    scrim.addEventListener("click", closeSheet);
    close.addEventListener("click", closeSheet);

    document.addEventListener("keydown", function (event) {
      if (event.key === "Escape") closeSheet();
    });

    var select = document.getElementById("openwebrx-sdr-profiles-listbox");
    if (select) {
      select.addEventListener("change", updateCurrent);
      new MutationObserver(refreshAvailability).observe(select, { childList: true });
    }

    updateCurrent();
    refreshAvailability();

    if (window.location.hash === "#protocols") {
      setTimeout(openSheet, 0);
    }
    window.addEventListener("hashchange", function () {
      if (window.location.hash === "#protocols") openSheet();
    });
  }

  var root;
  var toastTimer;

  function openSheet() {
    root.classList.add("m3-open");
    document.getElementById("m3-fab").setAttribute("aria-expanded", "true");
    refreshAvailability();
    updateCurrent();
  }

  function closeSheet() {
    if (!root) return;
    root.classList.remove("m3-open");
    var fab = document.getElementById("m3-fab");
    if (fab) fab.setAttribute("aria-expanded", "false");
  }

  function selectProfile(id, title) {
    var select = document.getElementById("openwebrx-sdr-profiles-listbox");
    if (!select || !select.options.length) {
      toast("El receptor aún se está cargando");
      return;
    }
    var available = Array.prototype.some.call(select.options, function (option) {
      return option.value === id;
    });
    if (!available) {
      toast("Perfil no disponible en este equipo");
      return;
    }
    select.value = id;
    if (typeof window.sdr_profile_changed === "function") {
      window.sdr_profile_changed();
    } else {
      toast("No se pudo cambiar el perfil");
      return;
    }
    closeSheet();
    toast("Abriendo " + title);
    setTimeout(updateCurrent, 400);
  }

  function updateCurrent() {
    var select = document.getElementById("openwebrx-sdr-profiles-listbox");
    var label = document.getElementById("m3-current-profile");
    if (!select || !label) return;
    var option = select.options[select.selectedIndex];
    if (!option) {
      label.textContent = "Iniciando…";
      return;
    }
    label.textContent = titles[option.value] || option.text;
  }

  function refreshAvailability() {
    var select = document.getElementById("openwebrx-sdr-profiles-listbox");
    if (!select) return;
    var ids = Array.prototype.map.call(select.options, function (option) {
      return option.value;
    });
    var cards = document.querySelectorAll("#m3-root .m3-card");
    Array.prototype.forEach.call(cards, function (card) {
      var available = ids.indexOf(card.getAttribute("data-profile")) !== -1;
      card.classList.toggle("m3-disabled", !available);
      card.setAttribute("aria-disabled", available ? "false" : "true");
    });
    var count = document.getElementById("m3-profile-count");
    if (count) count.textContent = ids.length + " perfiles";
  }

  function toast(message) {
    var node = document.getElementById("m3-toast");
    if (!node) return;
    node.textContent = message;
    node.classList.add("m3-show");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      node.classList.remove("m3-show");
    }, 2400);
  }

  Plugins.material = Plugins.material || {};
  Plugins.material._version = 1;
  Plugins.material.init = function () {
    if (document.getElementById("m3-root")) return true;
    root = document.createElement("div");
    root.id = "m3-root";
    build(root);
    document.body.appendChild(root);
    setInterval(updateCurrent, 3000);
    fetch("/status.json")
      .then(function (response) { return response.json(); })
      .then(function (data) {
        var device = document.getElementById("m3-device");
        if (device && data.receiver && data.receiver.name) {
          device.textContent = data.receiver.name + " · RX";
        }
      })
      .catch(function () {});
    return true;
  };
})();
