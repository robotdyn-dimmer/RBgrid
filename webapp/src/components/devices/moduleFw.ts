/**
 * Module firmware through the hub (root 534ded9e) — the pure parts of the dialog: the version as
 * people read it, which bank an image goes to, whether a file fits, and the step it is on.
 */
import type { ModuleOta, ModuleFwProgress, SensorModule } from '../../types/sensors';

/** STD `fw` = image version 0x0001007B → «1.123 (0x7B)»; a BASIC byte stays as it is. */
export function fwLabel(m: Pick<SensorModule, 'fw' | 'line' | 'fw_name'>): string | null {
  // BASIC: the hub's readable version (afb58e03: 0x0B → «1.4.19»); STD keeps «1.123 (0x7B)».
  if (m.line === 'basic' && m.fw_name) return m.fw_name;
  const fw = m.fw;
  if (fw == null) return null;
  if (fw > 0xffff) {
    const build = fw & 0xffff;
    return `${fw >>> 16}.${build} (0x${build.toString(16).toUpperCase().padStart(2, '0')})`;
  }
  // BASIC: a VERSION byte (root d03f19ed: 11 = 0x0B, not «1.1»).
  return `0x${fw.toString(16).toUpperCase().padStart(2, '0')}`;
}

const sameAddr = (a: string, b: string) => parseInt(a, 16) === parseInt(b, 16);

/** The bank an image is written to: the hub's `inactive_bank`, else the other of `banks`; null = unknown. */
export function targetBank(ota: ModuleOta | undefined): string | null {
  if (!ota) return null;
  if (ota.inactive_bank && !sameAddr(ota.inactive_bank, ota.active_bank)) return ota.inactive_bank;
  const other = ota.banks?.find((b) => !sameAddr(b, ota.active_bank));
  return other ?? null;
}

/** Why a chosen file can't go (null = fits). The hub checks the same and more (board, signature). */
export function fileProblem(file: { name: string; size: number }, ota: ModuleOta): 'ext' | 'size' | null {
  if (!/\.ota$/i.test(file.name)) return 'ext';
  if (file.size !== ota.bank_size) return 'size';
  return null;
}

export const FW_STEPS = ['writing', 'committing', 'activating', 'done'] as const;

/** 0–100 over the whole update: writing is ~90 % of the time, commit and activation the rest. */
export function fwPercent(p: ModuleFwProgress): number {
  if (p.state === 'done') return 100;
  if (p.state === 'writing') return p.total ? Math.min(90, Math.round((90 * (p.done ?? 0)) / p.total)) : 0;
  if (p.state === 'committing') return 92;
  if (p.state === 'activating') return 95;
  return 0;
}

export const fwRunning = (p: ModuleFwProgress | null): boolean =>
  !!p && (p.state === 'writing' || p.state === 'committing' || p.state === 'activating');

/** SHA-256 of the image as hex for X-FW-SHA256 — only where the browser allows (secure context). */
export async function sha256Hex(buf: ArrayBuffer): Promise<string | undefined> {
  try {
    const subtle = globalThis.crypto?.subtle;
    if (!subtle) return undefined;
    const h = await subtle.digest('SHA-256', buf);
    return Array.from(new Uint8Array(h)).map((b) => b.toString(16).padStart(2, '0')).join('');
  } catch { return undefined; }
}

/** What rbamp-dev's `.manifest` says about its sealed `.ota` (the image itself can't be read). */
export interface FwManifest {
  /** Bank as written: «A» / «B» (rbamp-dev's files) or an address. */
  bank?: string;
  board?: string;
  /** Image version (0x0001007B). */
  version?: number;
  variant?: string;
  /** The .ota it describes. */
  file?: string;
  sha256?: string;
}

/**
 * `.manifest` → its fields. rbamp-dev writes `key=value` lines (version=0x0001007B, variant=UI1,
 * board=0x42440211, bank=A, file=ui1_bankA_v0001007B.ota — v0001007B-UI1, 17.10); `key: value`
 * and JSON are read too.
 */
export function parseManifest(text: string): FwManifest {
  let kv: Record<string, string> = {};
  try {
    const j = JSON.parse(text);
    if (j && typeof j === 'object') kv = Object.fromEntries(Object.entries(j).map(([k, v]) => [k.toLowerCase(), String(v)]));
  } catch {
    for (const line of text.split(/\r?\n/)) {
      const m = /^\s*([A-Za-z_][\w.-]*)\s*[=:]\s*(.+?)\s*$/.exec(line);
      if (m && !line.trim().startsWith('#')) kv[m[1].toLowerCase()] = m[2].replace(/^["']|["']$/g, '');
    }
  }
  const hex8 = (v?: string) => (v && /^(0x)?[0-9a-f]{3,8}$/i.test(v) ? `0x${v.replace(/^0x/i, '').toUpperCase().padStart(8, '0')}` : undefined);
  const out: FwManifest = {};
  const bank = kv.bank?.trim();
  if (bank && /^[AB]$/i.test(bank)) out.bank = bank.toUpperCase();
  else if (hex8(bank)) out.bank = hex8(bank);
  const board = hex8(kv.board ?? kv.board_word);
  if (board) out.board = board;
  const ver = kv.version ? parseInt(kv.version, kv.version.toLowerCase().startsWith('0x') ? 16 : 10) : NaN;
  if (Number.isFinite(ver) && ver > 0) out.version = ver;
  if (kv.variant) out.variant = kv.variant;
  if (kv.file) out.file = kv.file;
  const sha = kv.sha256 ?? kv.sha;
  if (sha && /^[0-9a-f]{64}$/i.test(sha)) out.sha256 = sha.toLowerCase();
  return out;
}

const hexEq = (a?: string, b?: string) => !!a && !!b && parseInt(a, 16) === parseInt(b, 16);

/** The manifest's bank as an address: «A» = the lower of `ota.banks`, «B» = the upper; null = can't tell. */
export function manifestBankAddr(mf: FwManifest, ota: ModuleOta): string | null {
  if (!mf.bank) return null;
  if (mf.bank === 'A' || mf.bank === 'B') {
    const banks = [...(ota.banks ?? [])].sort((a, b) => parseInt(a, 16) - parseInt(b, 16));
    return banks.length === 2 ? banks[mf.bank === 'A' ? 0 : 1] : null;
  }
  return mf.bank;
}

/** A manifest that doesn't match the chosen file, the free bank or the module's board — refused before writing. */
export function manifestProblem(mf: FwManifest, ota: ModuleOta, bank: string | null, fileName?: string): 'file' | 'bank' | 'board' | null {
  if (mf.file && fileName && mf.file !== fileName) return 'file';
  const addr = manifestBankAddr(mf, ota);
  if (addr && bank && !hexEq(addr, bank)) return 'bank';
  if (mf.board && ota.board && !hexEq(mf.board, ota.board)) return 'board';
  return null;
}
