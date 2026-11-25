type PageElements = {
  '#prompt': HTMLTextAreaElement;
  '#username-form': HTMLFormElement;
  '#auth-form': HTMLFormElement;
  '#chat-form': HTMLFormElement;
  '#password-form': HTMLFormElement;
  '#email-form': HTMLFormElement;
  '#email-verify-form': HTMLFormElement;
  '#logout-all-form': HTMLFormElement;
  '#logout-device-form': HTMLFormElement;
} & Record<`#auth-${'username' | 'identifier' | 'email' | 'code' | 'password' | 'confirm'}`, HTMLInputElement>
  & Record<`#${'send' | 'stop' | 'retry' | 'model-select' | 'devices-more' | 'auth-submit'}`, HTMLButtonElement>;

export function $<S extends string>(selector: S): S extends keyof PageElements ? PageElements[S] : HTMLElement;
export function $<T extends HTMLElement>(selector: string): T;
export function $(selector: string): HTMLElement {
  const element = document.querySelector(selector);
  if (!(element instanceof HTMLElement)) throw new Error(`页面缺少元素：${selector}`);
  return element;
}

export function formInput(form: HTMLFormElement, name: string): HTMLInputElement {
  const input = form.elements.namedItem(name);
  if (!(input instanceof HTMLInputElement)) throw new Error(`表单缺少输入框：${name}`);
  return input;
}
