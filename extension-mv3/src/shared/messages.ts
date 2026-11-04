// Сообщения popup -> service worker. Единственное место, где описан этот контракт.
import type { ProductPayload } from './extract';

export type PopupRequest =
  | { type: 'status' }
  | { type: 'pair'; code: string }
  | { type: 'save-settings'; storeId: number | null }
  | { type: 'import'; product: ProductPayload }
  | { type: 'sync-cookies'; pageUrl: string }
  | { type: 'update-check' }
  | { type: 'update-apply' }
  | { type: 'orders-list' }
  | { type: 'order-result'; orderId: number; status: 'ordered' | 'failed' | 'needs_manual'; supplierOrderId?: string; message?: string }
  | { type: 'order-track'; orderId: number; trackingNumber: string; carrier?: string };

export interface StatusReply {
  connected: boolean;
  hasToken: boolean;
  storeId: number | null;
  error?: string;
}

export type PopupReply<T = unknown> = { ok: true; data: T } | { ok: false; error: string };

export interface ImportResult {
  product_id: number;
  duplicate: boolean;
  variants: number;
}

export interface UpdateInfo {
  current: string;
  latest: string;
  update_available: boolean;
  release_url: string;
  notes: string;
  can_apply: boolean;
}
