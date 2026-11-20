# mkfixtures.py — regenera tests/data/real/*.sigmf-{meta,data} a partir de los
# vectores reales ya versionados en tests/data/ (recortes de grabaciones públicas), salvo
# flex3200 y flex6400, que salen de los MP3 originales de sigidwiki (se descargan a build/fuentes_real
# y se verifica su sha256; ffmpeg solo se usa para decodificar el MP3).
# Uso: python3 -I tests/data/real/mkfixtures.py   (desde la raíz del repositorio)
# Requiere numpy, scipy y ffmpeg (solo para regenerar; el test no los necesita).
import hashlib, json, os, subprocess, urllib.request, numpy as np
from scipy.signal import resample_poly

D = "tests/data"; O = "tests/data/real"
DL = "build/fuentes_real"   # caché de originales descargados (build/ está en .gitignore)

def save(id_, iq, dt, rate, freq, author, desc, shift_hz=None):
    """iq: complejo float en ±1; dt: 'ci8' | 'cu8' | 'ci16_le'.
    shift_hz: el audio remodulado deja la portadora en 0 Hz y el motor quita la continua
    (la AM de ACARS, p. ej., desaparece); se desplaza la señal +shift_hz y el centro
    SigMF se baja otro tanto, de modo que el canal sigue en 'freq'."""
    if shift_hz is None:
        shift_hz = 10000 if rate == 48000 else 0
    if shift_hz:
        iq = iq * np.exp(2j * np.pi * shift_hz * np.arange(len(iq)) / rate)
        freq = freq - shift_hz
    if dt == "ci8":
        a = np.empty(2 * len(iq), np.int8)
        a[0::2] = np.clip(np.round(iq.real * 127), -127, 127); a[1::2] = np.clip(np.round(iq.imag * 127), -127, 127)
    elif dt == "ci16_le":
        a = np.empty(2 * len(iq), "<i2")
        a[0::2] = np.clip(np.round(iq.real * 32767), -32767, 32767); a[1::2] = np.clip(np.round(iq.imag * 32767), -32767, 32767)
    else:
        a = np.empty(2 * len(iq), np.uint8)
        a[0::2] = np.clip(np.round(iq.real * 127.5 + 127.5), 0, 255); a[1::2] = np.clip(np.round(iq.imag * 127.5 + 127.5), 0, 255)
    a.tofile(f"{O}/{id_}.sigmf-data")
    meta = {"global": {"core:datatype": dt, "core:sample_rate": int(rate), "core:version": "1.0.0",
                       "core:author": author, "core:description": desc,
                       "core:license": "CC-BY-SA-3.0 (sigidwiki)" if "sigidwiki" in author else "GPL-2.0 (solo datos de prueba)" if "GPL-2.0" in author else "sin licencia explicita: datos de prueba locales de una grabacion publica; se conserva solo el recorte minimo" if "rs1729" in author else "generada localmente (sin restricciones)"},
            "captures": [{"core:sample_start": 0, "core:frequency": int(freq)}], "annotations": []}
    json.dump(meta, open(f"{O}/{id_}.sigmf-meta", "w"), indent=2, ensure_ascii=False)

def cs8(f): a = np.fromfile(f, np.int8).astype(np.float32); return (a[0::2] + 1j * a[1::2]) / 127
def cu8(f): a = np.fromfile(f, np.uint8).astype(np.float32); return ((a[0::2] - 127.5) + 1j * (a[1::2] - 127.5)) / 127.5
def phase(f): p = np.fromfile(f, np.uint8).astype(np.float64); return 0.8 * np.exp(2j * np.pi * p / 256)

# Originales MP3 de sigidwiki (CC BY-SA 3.0): nombre -> (URL pública, sha256 del fichero).
FUENTES = {
    "Flex_3200.mp3": ("https://www.sigidwiki.com/images/5/58/Flex_3200.mp3",
                      "a49941e22179cdf2caf2e73e596b8d786d3ff0e3250822465a8239f736f9706f"),
    "FLEX_6400bps.mp3": ("https://www.sigidwiki.com/images/0/00/FLEX_6400bps.mp3",
                         "4fedc3352ffcbac0c14dcf28c2ee433b74338c1a0c5972d87ff1baabbc27c843"),
}

def fuente(nombre):
    """Ruta local del original; lo descarga si falta o si su sha256 no coincide."""
    url, sha = FUENTES[nombre]
    dst = os.path.join(DL, nombre)
    if os.path.exists(dst):
        with open(dst, "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() == sha:
                return dst
    os.makedirs(DL, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "bladerf-lab-mkfixtures/1.0"})  # el wiki rechaza el UA por defecto
    with urllib.request.urlopen(req, timeout=120) as r:
        datos = r.read()
    if hashlib.sha256(datos).hexdigest() != sha:
        raise SystemExit(f"sha256 inesperado descargando {nombre}")
    with open(dst, "wb") as f:
        f.write(datos)
    return dst

def fm_desde_mp3(nombre, t0, t1, dev_hz):
    """Recorte [t0, t1) s del audio de un MP3 de sigidwiki, remodulado en FM a 48 kHz: la desviación
    de pico (percentil 99,5 del recorte) es dev_hz y la amplitud 0,8, igual que en las demás muestras
    de audio. Se quita la continua antes (la mediana) para que la portadora quede en 0 Hz."""
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", fuente(nombre), "-ar", "48000",
                          "-ac", "1", "-f", "f32le", "-"], capture_output=True, check=True).stdout
    x = np.frombuffer(raw, dtype=np.float32).astype(np.float64)
    x = x[int(t0 * 48000):int(t1 * 48000)]
    x = x - np.median(x)
    pk = np.percentile(np.abs(x), 99.5)
    ph = 2 * np.pi * np.cumsum(x / pk * dev_hz) / 48000.0
    return 0.8 * np.exp(1j * ph)

SIGID = "Signal Identification Wiki (sigidwiki.com), CC BY-SA 3.0"
MM = "multimon-ng test/samples (EliasOenal/multimon-ng), GPL-2.0, solo datos"
RTL = "rtl_433_tests (merbanan/rtl_433_tests), GPL-2.0, solo datos"

save("acars", cs8(f"{D}/acars_sigid_am.cs8"), "ci8", 48000, 131525000, SIGID,
     "ACARS Nueva Zelanda (ZK-OKM, NZ0006), audio AM de 'ACARS sound' remodulado en AM m=0,7, recorte de 2-3,2 s")
save("ais", cs8(f"{D}/ais_la_fm48k.cs8"), "ci8", 48000, 161975000, SIGID,
     "AIS Long Beach, audio FM de 'AIS Sound' remodulado en FM, recorte")
save("aprs", phase(f"{D}/aprs_afsk1200_phase.u8"), "ci8", 48000, 144800000, SIGID,
     "AFSK1200 (BBS FBB, ZL1AB) de 'AFSK1200 Sound', audio FM remodulado en FM +-3 kHz, 4 s")
save("flex", phase(f"{D}/flex_p2000_phase.u8"), "ci8", 48000, 466075000, MM,
     "FLEX 1600 2FSK P2000 (FLEX_1600_2fsk_P2000_proef_alarm.flac) remodulado en FM")
# shift_hz=0: el audio ya se remodula con la portadora en 0 Hz y no se desplaza (la muestra queda en 466,075 MHz).
save("flex3200", fm_desde_mp3("Flex_3200.mp3", 0.0, 2.2, 4800), "ci8", 48000, 466075000, SIGID,
     "FLEX 3200 bit/s (4FSK, 1600 Bd, fases A y B, sync 0xB068), audio FM de 'Flex 3200' (sigidwiki) remodulado en FM +-4,8 kHz, recorte de 0-2,2 s (tramas ciclo 8, 117 y 118); multimon-ng no la decodifica",
     shift_hz=0)
save("flex6400", fm_desde_mp3("FLEX_6400bps.mp3", 5.2, 7.2, 4800), "ci8", 48000, 466075000, SIGID,
     "FLEX 6400 bit/s (4FSK, 3200 Bd, fases A-D, sync 0xDEA0, direcciones largas), audio FM de 'FLEX 6400bps' (sigidwiki) remodulado en FM +-4,8 kHz, recorte de 5,2-7,2 s (trama ciclo 4, nº 56)",
     shift_hz=0)
for b in (512, 1200, 2400):
    save(f"pocsag{b}", phase(f"{D}/pocsag{b}_mm_phase.u8"), "ci8", 48000, 466075000, MM,
         f"POCSAG {b} bps (POCSAG_sample_-_{b}_bps.flac) remodulado en FM +-4,5 kHz")
au = np.fromfile(f"{D}/rs41_demod_20140717.u8", np.uint8).astype(np.float64) - 128
hist = np.abs(au).astype(int); p90 = max(1, int(np.percentile(hist, 90)))
ph = np.cumsum(2 * np.pi * 2400 * (au / p90) / 48000)
save("rs41", 0.5 * np.exp(1j * ph), "ci8", 48000, 403000000, "rs1729/RS (github.com/rs1729/RS), audio FM demodulado de una sonda real, solo datos",
     "RS41 K1930308 (2014-07-17), 1,9 s de audio demodulado remodulado en FM +-2,4 kHz")
g = cu8(f"{D}/gsm_sigid_bcch_300k.cu8")
save("gsm", resample_poly(g, 65, 18).astype(np.complex64), "ci8", 1083333, 947000000, SIGID,
     "GSM downlink BCCH (BSIC 23), 0,25 s a 300 kS/s remuestreada a 1,0833 MS/s (65/18)")
save("lte", cs8(f"{D}/lte_sigid_vf14_1p92M.cs8"), "ci8", 1920000, 800000000, SIGID,
     "LTE 1,4 MHz (PCI 65), 50 ms de 'LTE_VF_1.4_downlink' remuestreados a 1,92 MS/s")
save("wifi", cs8(f"{D}/wifi_sigid_beacon_20M.cs8"), "ci8", 20000000, 2437000000, SIGID,
     "Wi-Fi 2,4 GHz canal 1, baliza DSSS 1 Mb/s (SSID UPC8443073.2,4G), 3,8 ms de 'Wi-Fi.zip' a 20 MS/s")
for n, f in [("acurite592txr", "ism_acurite592txr_rtl433"), ("nexus", "ism_nexus_rtl433"), ("f007th", "ism_f007th_rtl433"),
             ("wh1080", "ism_wh1080_rtl433"), ("oregon_thn132n", "ism_oregon_thn132n_rtl433"),
             ("oregon_thgr122n", "ism_oregon_thgr122n_rtl433"), ("tx141bv2", "ism_tx141bv2_rtl433"), ("tx141bv3", "ism_tx141bv3_rtl433")]:
    save(f"ism_{n}", cu8(f"{D}/{f}.cu8"), "cu8", 250000, 433920000, RTL, f"ISM 433,92 MHz, {n}, IQ cu8 a 250 kS/s tal cual")
a = np.fromfile(f"{D}/lora_loratx_sf7_cr5_bw125.cs16", "<i2").astype(np.float32) / 32768
save("lora", a[0::2] + 1j * a[1::2], "ci16_le", 500000, 868100000, "dxlaprs-lora loratx (transmisor independiente), generada localmente",
     "LoRa SF7 CR4/5 BW125, red 0x34, carga 'Hola desde loratx 123'")
