import { describe, expect, it, vi } from 'vitest';
import { RpcFailure } from '../src/shared/bridge';
import { addressLines, handleAutoOrder, needsAction, parseAutoOrder } from '../src/shared/orders';

const good = { order_id: 7, url: 'https://shop.example/item/1', sku: 'SKU-1', quantity: 2, address: { city: 'Тула' }, auto_pay: false };

describe('assisted purchase', () => {
  it('opens the supplier page and accepts the command', async () => {
    const deps = { openTab: vi.fn().mockResolvedValue(undefined), notify: vi.fn().mockResolvedValue(undefined) };
    await expect(handleAutoOrder(good, deps)).resolves.toEqual({ accepted: true, mode: 'assisted' });
    expect(deps.openTab).toHaveBeenCalledWith('https://shop.example/item/1');
    expect(deps.notify).toHaveBeenCalledOnce();
  });

  it('refuses auto_pay: the extension never pays', () => {
    expect(() => parseAutoOrder({ ...good, auto_pay: true })).toThrow(RpcFailure);
  });

  it.each([
    [{ ...good, order_id: 0 }],
    [{ ...good, quantity: 0 }],
    [{ ...good, url: 'javascript:alert(1)' }],
    [{ ...good, url: 'file:///etc/passwd' }],
    [{ ...good, url: 'not a url' }],
    [{ ...good, sku: 5 }],
    [null],
  ])('rejects bad params without opening a tab (%#)', async (params) => {
    const deps = { openTab: vi.fn(), notify: vi.fn() };
    await expect(handleAutoOrder(params, deps)).rejects.toMatchObject({ code: -32602 });
    expect(deps.openTab).not.toHaveBeenCalled();
  });

  it('formats the address and filters actionable orders', () => {
    expect(addressLines({ name: 'Иван', city: 'Тула', empty: '', nested: { a: 1 }, none: null })).toBe('name: Иван\ncity: Тула');
    expect(needsAction({ status: 'QUEUED_FOR_EXT' })).toBe(true);
    expect(needsAction({ status: 'SHIPPED' })).toBe(false);
  });
});
