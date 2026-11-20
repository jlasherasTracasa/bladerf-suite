#!/usr/bin/env python3
"""plots_d.py — «estructura de la trama» (campos coloreados) del resto de protocolos.
Ilustraciones dibujadas a partir de los estándares citados y de docs/protocolos/*.md (no son capturas).
Uso: python3 -I plots_d.py"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plots_common import *

frame_render("dab-estructura", "DAB modo I: trama de transmisión de 96 ms", [
    ("Trama de transmisión", [("símbolo nulo", 5, "2656 muestras"), ("PRS", 5, "referencia de fase"), ("FIC", 15, "3 símbolos = 12 FIB"), ("MSC", 75, "72 símbolos: subcanales de audio y datos")]),
    ("FIB (32 B) del FIC", [("FIG", 60, "FIG 0/0 conjunto, 0/1 subcanales, 0/2 servicios, 1/x etiquetas"), ("relleno", 8, "0xFF"), ("CRC-16", 10, "")]),
], "Estructura según ETSI EN 300 401 y docs/protocolos/dab.md. Ilustración dibujada.", rh=1.2)
frame_render("dsc-estructura", "DSC: llamada de socorro (FSK 1200 bd, símbolos de 10 bits)", [
    ("Secuencia de símbolos", [("sincronía", 12, "dot ×200 + fase"), ("formato", 5, "×2"), ("dirección", 10, "MMSI de 5 símbolos"), ("categoría", 4, "×1"), ("auto-ID", 10, "MMSI propio"), ("telemando", 6, "naturaleza"), ("posición", 12, "10 símbolos"), ("hora", 5, "UTC"), ("EOS", 4, "×3"), ("ECC", 4, "paridad")]),
], "Estructura según ITU-R M.493 y docs/protocolos/dsc.md. Cada símbolo: 7 bits de información + 3 de comprobación; cada uno se envía dos veces entrelazado (DX y RX). Ilustración dibujada.", rh=1.5)
frame_render("aprs-estructura", "APRS: trama AX.25 UI sobre HDLC", [
    ("Trama en el aire", [("bandera", 4, "0x7E"), ("destino", 14, "7 B"), ("origen", 14, "7 B"), ("digipeaters", 14, "0-8 × 7 B"), ("control", 3, "0x03"), ("PID", 3, "0xF0"), ("información", 30, "APRS: posición, estado, mensaje…"), ("FCS", 6, "CRC-16"), ("bandera", 4, "0x7E")]),
    ("Información de posición (!)", [("tipo", 3, "!"), ("lat", 10, "DDMM.mmN"), ("tabla", 3, "/"), ("lon", 12, "DDDMM.mmW"), ("símbolo", 4, ">"), ("comentario", 20, "texto libre")]),
], "Estructura según AX.25 v2.2 y APRS 1.0.1; docs/protocolos/aprs.md. Ilustración dibujada.", rh=1.2)
frame_render("flex-estructura", "FLEX: trama de 1,875 s (1600 bd, 2 niveles)", [
    ("Trama", [("Sync 1", 6, "A + patrón + FIW"), ("Sync 2", 3, "velocidad"), ("bloque 0…10", 60, "8 palabras × 32 bits, entrelazadas en 4 fases (A-D)"), ("fase", 6, "")]),
    ("Palabras del bloque", [("BIW", 6, "info. del bloque"), ("direcciones", 14, "1 o 2 palabras"), ("vectores", 14, "tipo, inicio, longitud"), ("mensaje", 30, "texto 7 bits")]),
], "Estructura según docs/protocolos/flex.md (protocolo FLEX de Motorola, descrito por multimon-ng y la literatura). Ilustración dibujada.", rh=1.2)
frame_render("dmr-estructura", "DMR: ráfaga TDMA de 30 ms (264 bits = 132 símbolos de 4FSK)", [
    ("Ráfaga de datos", [("CACH", 12, "24 bits"), ("carga", 54, "98 bits BPTC"), ("slot type", 10, "20 bits"), ("sincronía", 24, "48 bits"), ("slot type", 10, ""), ("carga", 54, "98 bits")]),
    ("Contenido del metadato (sin voz)", [("CC", 6, "color code"), ("ranura", 6, "1 o 2"), ("tipo", 10, "cabecera, datos, terminador"), ("LC", 30, "origen, destino, FLCO"), ("FEC", 12, "RS/CRC")]),
], "Estructura según ETSI TS 102 361-1 y docs/protocolos/dmr.md. No se decodifica voz. Ilustración dibujada.", rh=1.2)
frame_render("vdl2-estructura", "VDL Modo 2: ráfaga D8PSK a 10 500 bd", [
    ("Ráfaga", [("rampa", 3, ""), ("entrenamiento", 14, "UW de 16 símbolos"), ("cabecera", 8, "reservado + longitud (17 b) + paridad (5 b)"), ("carga (aleatorizada)", 60, "bloques Reed-Solomon"), ("FEC", 15, "RS")]),
    ("Trama AVLC dentro de la carga", [("bandera", 4, "0x7E"), ("destino", 12, "27 bits"), ("origen", 12, "27 bits"), ("control", 6, ""), ("información", 40, "ACARS sobre AVLC"), ("FCS", 8, "CRC-16"), ("bandera", 4, "0x7E")]),
], "Estructura según ETSI EN 301 841-1 y docs/protocolos/vdl2.md. Ilustración dibujada.", rh=1.2)
frame_render("rs41-estructura", "RS41: trama de 312 octetos (GFSK 4800 bd, desaleatorizada)", [
    ("Trama", [("cabecera", 8, "64 bits"), ("estado", 14, "n.º de serie, trama, batería"), ("PTU", 16, "presión, temperatura, humedad"), ("GPS-pos", 18, "ECEF"), ("GPS-raw", 14, "pseudodistancias"), ("extra", 14, ""), ("RS", 16, "24 octetos de paridad por bloque")]),
], "Estructura según docs/protocolos/rs41.md (rs1729/RS, bloques con CRC-16 y Reed-Solomon GF(256), 0x11D). Ilustración dibujada.", rh=1.5)
frame_render("lte-estructura", "LTE FDD: dónde están PSS, SSS y PBCH (trama de 10 ms)", [
    ("Subtramas 0-4 (5 ms)", [("sub 0", 20, "PSS/SSS ranura 0/1, PBCH ranura 1"), ("sub 1", 12, ""), ("sub 2", 12, ""), ("sub 3", 12, ""), ("sub 4", 12, "")]),
    ("Símbolos de la ranura 0", [("0-4", 20, "datos / CRS"), ("5 · SSS", 6, "62 sub., ±1"), ("6 · PSS", 6, "62 sub., ZC raíz 25/29/34")]),
    ("PBCH (subtrama 0, ranura 1)", [("símbolos 0-3", 20, "72 sub. centrales; MIB 24 bits + CRC-16 → 480 bits QPSK"), ("SFN mod 4", 8, "por hipótesis")]),
], "Estructura según 3GPP TS 36.211 §6.11 y docs/protocolos/lte.md. Ilustración dibujada.", rh=1.0)
frame_render("nr-estructura", "5G NR: bloque SSB (4 símbolos × 240 subportadoras)", [
    ("Símbolos OFDM del SSB", [("0 · PSS", 12, "subportadoras 56-182"), ("1 · PBCH", 20, "240 sub."), ("2 · PBCH + SSS", 28, "SSS en 56-182"), ("3 · PBCH", 20, "240 sub.")]),
    ("Identidad de celda", [("PSS", 12, "N_ID2 = 0, 1, 2"), ("SSS", 20, "N_ID1 = 0…335"), ("PCI", 16, "3·N_ID1 + N_ID2")]),
], "Estructura según 3GPP TS 38.211 §7.4.2 y docs/protocolos/nr.md. Ilustración dibujada.", rh=1.2)
frame_render("gsm-estructura", "GSM: multitrama de control de 51 tramas (BCCH, 235 ms)", [
    ("Ranura 0 de las 51 tramas (TS0 del canal de difusión)", [("F", 4, "0"), ("B0-B3", 14, "BCCH 2-5"), ("PCH/AGCH", 14, ""), ("F S", 6, "10 · 11"), ("BCCH", 14, ""), ("F S", 6, "20 · 21"), ("F S", 6, "30 · 31"), ("F S", 6, "40 · 41"), ("libre", 4, "50")]),
    ("Ráfaga normal (148 bits, 577 µs)", [("cola", 3, "3"), ("datos", 57, "57"), ("secuencia de aprendizaje", 26, "TSC = BCC"), ("datos", 57, "57"), ("cola", 3, "3")]),
], "Estructura según 3GPP TS 45.002 y docs/protocolos/gsm.md. F = FCCH (tono), S = SCH (BSIC y n.º de trama). Ilustración dibujada.", rh=1.3)
frame_render("wifi-estructura", "WiFi 802.11: baliza (beacon) y su capa física", [
    ("PPDU OFDM (11a/g)", [("STF", 8, "10 símbolos cortos"), ("LTF", 8, "2 largos"), ("SIGNAL", 4, "RATE, LEN"), ("PSDU", 40, "trama MAC")]),
    ("Trama de gestión Beacon", [("FC", 4, "subt. 8"), ("destino", 6, "difusión"), ("origen", 6, ""), ("BSSID", 6, "se emite"), ("marca/intervalo", 8, "TSF, 102,4 ms"), ("elementos", 30, "SSID, tasas, canal, RSN"), ("FCS", 4, "CRC-32")]),
], "Estructura según IEEE 802.11 y docs/protocolos/wifi.md. Solo se leen BSSID y elementos públicos (alcance ético §0.12). Ilustración dibujada.", rh=1.3)
frame_render("tdt-estructura2", "TDT: cadena de capas de la señal de televisión", [
    ("De la antena a la imagen", [("OFDM 8k", 10, "DVB-T, 64-QAM"), ("FEC", 10, "Viterbi + RS(204,188)"), ("TS 188 B", 10, "multiplex"), ("PES", 10, "paquetes elementales"), ("H.264", 14, "vídeo (navegador)"), ("MP2", 8, "audio propio")]),
], "Resumen de docs/protocolos/tdt.md. Ilustración dibujada.", rh=1.5)
