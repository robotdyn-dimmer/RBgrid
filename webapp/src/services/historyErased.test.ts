import { dropErasedRecords } from './api';

describe('history: erased-flash records are not hours', () => {
  it('drops ts/dur 0xFFFFFFFF · res 255 slots, keeps real hours and the ts=0 shelf', () => {
    const erased = { ts: 4294967295, dur: 4294967295, q: 65535, res: 255, ec: -0.001 };
    const hour = { ts: 1792321200, dur: 3600, q: 0, res: 1, ec: 402.468 };
    const shelf = { ts: 0, dur: 600, q: 1, res: 1, ec: 3 };
    expect(dropErasedRecords([erased, hour, erased, shelf])).toEqual([hour, shelf]);
  });
});
