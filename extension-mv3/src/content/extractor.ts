// Внедряется в страницу только по клику пользователя (activeTab); постоянных скриптов нет.
import { extractProduct } from '../shared/extract';

(globalThis as any).__dropshipExtract = () => extractProduct(document, location.href);
