import { fwLabel, targetBank, fileProblem, fwPercent, fwRunning, parseManifest, manifestProblem, manifestBankAddr } from './moduleFw';

describe('module firmware helpers', () => {
  it('reads the STD image version and leaves a BASIC byte', () => {
    expect(fwLabel({ fw: 65659, line: 'std' })).toBe('1.123 (0x7B)');
    expect(fwLabel({ fw: 11, line: 'basic' })).toBe('0x0B'); // BASIC: a VERSION byte (root d03f19ed)
    expect(fwLabel({ fw: 11, line: 'basic', fw_name: '1.4.19' })).toBe('1.4.19'); // afb58e03
    expect(fwLabel({ line: 'std' })).toBeNull();
  });
  it('picks the bank that is not running', () => {
    expect(targetBank({ bank_size: 1, active_bank: '0x08030000', inactive_bank: '0x08004000' })).toBe('0x08004000');
    expect(targetBank({ bank_size: 1, active_bank: '0x08030000', banks: ['0x8030000', '0x08004000'] })).toBe('0x08004000');
    expect(targetBank({ bank_size: 1, active_bank: '0x08030000', inactive_bank: '0x08030000' })).toBeNull();
    expect(targetBank({ bank_size: 1, active_bank: '0x08030000' })).toBeNull();
    expect(targetBank(undefined)).toBeNull();
  });
  it('checks the file name and size', () => {
    const ota = { bank_size: 180224, active_bank: '0x08030000' };
    expect(fileProblem({ name: 'ui1-7b.ota', size: 180224 }, ota)).toBeNull();
    expect(fileProblem({ name: 'ui1-7b.bin', size: 180224 }, ota)).toBe('ext');
    expect(fileProblem({ name: 'ui1-7b.OTA', size: 1000 }, ota)).toBe('size');
  });
  it('maps progress over the whole update', () => {
    expect(fwPercent({ module: 0, state: 'writing', done: 352, total: 704 })).toBe(45);
    expect(fwPercent({ module: 0, state: 'activating' })).toBe(95);
    expect(fwPercent({ module: 0, state: 'done' })).toBe(100);
    expect(fwRunning({ module: 0, state: 'committing' })).toBe(true);
    expect(fwRunning({ module: 0, state: 'error' })).toBe(false);
  });
});

describe('the .manifest', () => {
  const ota = { bank_size: 180224, active_bank: '0x08030000', inactive_bank: '0x08004000', banks: ['0x08004000', '0x08030000'], board: '0x42440211' };
  // rbamp-dev's own file, v0001007B-UI1 (17.10)
  const real = 'version=0x0001007B\nvariant=UI1\nboard=0x42440211\nbank=A\nfile=ui1_bankA_v0001007B.ota\n';
  it('reads rbamp-dev key=value, key: value and JSON', () => {
    expect(parseManifest(real)).toEqual({ bank: 'A', board: '0x42440211', version: 65659, variant: 'UI1', file: 'ui1_bankA_v0001007B.ota' });
    expect(parseManifest('bank: 8004000\nboard: "42440211"')).toEqual({ bank: '0x08004000', board: '0x42440211' });
    expect(parseManifest('{"bank":"0x08004000","sha256":"' + 'A'.repeat(64) + '"}')).toEqual({ bank: '0x08004000', sha256: 'a'.repeat(64) });
  });
  it('maps bank A / B onto the module banks', () => {
    expect(manifestBankAddr({ bank: 'A' }, ota)).toBe('0x08004000');
    expect(manifestBankAddr({ bank: 'B' }, ota)).toBe('0x08030000');
    expect(manifestBankAddr({ bank: 'B' }, { ...ota, banks: undefined })).toBeNull();
  });
  it('refuses another file, the other bank or another board', () => {
    const mf = parseManifest(real);
    expect(manifestProblem(mf, ota, '0x08004000', 'ui1_bankA_v0001007B.ota')).toBeNull();
    expect(manifestProblem(mf, ota, '0x08004000', 'ui1_bankB_v0001007B.ota')).toBe('file');
    expect(manifestProblem({ ...mf, bank: 'B', file: undefined }, ota, '0x08004000', 'x.ota')).toBe('bank');
    expect(manifestProblem({ board: '0x42440863' }, ota, '0x08004000')).toBe('board');
  });
});
