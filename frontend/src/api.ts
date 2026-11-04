// Единственное место, где интерфейс говорит с ядром. Ошибки ядра приходят уже понятным русским текстом.
import { invoke } from '@tauri-apps/api/core';

export const errorText = (e: unknown): string => (typeof e === 'string' ? e : e instanceof Error ? e.message : 'Что-то пошло не так');

export async function call<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  try {
    return await invoke<T>(command, args);
  } catch (e) {
    throw new Error(errorText(e));
  }
}

export interface ProductRow {
  id: number;
  title: string;
  raw_title: string;
  ai_status: string;
  monitor_status: string;
  supplier_url: string;
  variants: number;
  linked: number;
  min_retail_minor: number | null;
  stock: number;
  last_checked_at: number;
  image: string | null;
}

export interface Variant {
  id: number;
  supplier_sku: string;
  attributes_json: string;
  supplier_price_minor: number;
  shipping_price_minor: number;
  retail_price_minor: number;
  min_price_minor: number;
  old_price_minor: number;
  supplier_stock: number;
  is_in_stock: boolean;
  external_variant_id: string | null;
  sync_state: 'not_linked' | 'synced' | 'rejected' | 'pending';
}

export interface HistoryPoint {
  variant_id: number;
  supplier_price_minor: number;
  retail_price_minor: number;
  supplier_stock: number;
  recorded_at: number;
}

export interface ProductDetail {
  id: number;
  raw_title: string;
  raw_description: string | null;
  ai_title: string | null;
  ai_description: string | null;
  supplier_url: string;
  ai_status: string;
  monitor_status: string;
  variants: Variant[];
  history: HistoryPoint[];
}

export interface StoreRow {
  id: number;
  name: string;
  platform_type: string;
  api_url: string;
  warehouse_id: string | null;
  is_active: boolean;
  products: number;
  last_error: string | null;
}

export interface Rule {
  rule_name: string;
  price_from_minor: number;
  price_to_minor: number;
  margin_percent: number;
  fixed_markup_minor: number;
  marketplace_fee_percent: number;
  acquiring_and_tax_percent: number;
  discount_striken_percent: number;
  rounding_minor: number;
  min_stock_threshold: number;
  min_profit_minor: number;
}

export interface OrderRow {
  id: number;
  external_order_id: string;
  status: string;
  quantity: number;
  retail_total_cents: number;
  net_profit_cents: number | null;
  address: Record<string, unknown>;
  supplier_order_id: string | null;
  tracking_number: string | null;
  error_message: string | null;
  human_readable_hint: string | null;
  supplier_url: string | null;
  sku: string | null;
  title: string | null;
}

export interface Onboarding {
  done: boolean;
  has_store: boolean;
  has_rules: boolean;
  ai_configured: boolean;
  extension_paired: boolean;
}

export interface AiState {
  settings: { provider: string; model: string | null; base_url: string | null; language: string };
  has_key: boolean;
}

export interface UpdateInfo {
  current: string;
  latest: string;
  update_available: boolean;
  release_url: string;
  notes: string;
}
