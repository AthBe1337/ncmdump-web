# ncmdump-web

适用于Web页面的网易云加密格式NCM解密工具，直接在前端转换，无需后端交互。

由[taurusxin/ncmdump](https://github.com/taurusxin/ncmdump/)移植而来。

## 示例页面

这是一个简单的示例页面，可以直接使用。

[NCM 解密工具](https://tools.athbe.cn/ncm)

![示例页面](https://cloud.athbe.cn/f/N0Ij/I@%28XB15M5_UU0FY4FDPM~EU.png)



## 构建



安装WASM工具链（如果没有的话）

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
# 这条指令每次启动都需要执行，为了使它永久生效，可将其添加到~/.bashrc
source ./emsdk_env.sh
```

下载源码并编译

```bash
git clone https://github.com/AthBe1337/ncmdump-web.git
cd ncmdump-web
mkdir build && cd build
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
emmake make -j$(nproc)
```

此时你将得到`ncmdump.js`和`ncmdump.wasm`，你可以将它集成到你的web页面中。

## 调用方法示例

传入参数为缓冲区指针和长度，返回typed_memory_view（Uint8Array）。

`index.html`和`decrypt.worker.js`提供了一个简易的使用示例。以下是`decrypt.worker.js`。

```js
import createModule from './wasm/ncmdump.js';

let wasmModule = null;
let isWasmReady = false;

createModule({
    locateFile: (path) => path.endsWith('.wasm') ? './wasm/ncmdump.wasm' : path
}).then((instance) => {
    wasmModule = instance;
    isWasmReady = true;
    self.postMessage({ type: 'wasm-ready' });
}).catch((err) => {
    self.postMessage({
        type: 'error',
        error: `WASM加载失败: ${err.message}`
    });
});

self.onmessage = async (e) => {
    const { type, payload } = e.data;

    if (type === 'decrypt') {
        let inputDataPtr = null;
        try {
            if (!isWasmReady) {
                throw new Error("WASM模块尚未初始化完成");
            }

            const fileData = new Uint8Array(payload.fileData);
            const baseName = payload.baseNameWithoutExtension || "output";

            inputDataPtr = wasmModule._malloc(fileData.length);
            if (!inputDataPtr) {
                throw new Error("无法为输入文件数据分配WASM内存");
            }

            wasmModule.HEAPU8.set(fileData, inputDataPtr);

            const resultView = wasmModule.decryptNCM(inputDataPtr, fileData.length);

            if (resultView === null) {
                const cppError = wasmModule.getLastError();
                const cleanError = cppError.replace(/^std::\w+,\s*/, '');
                throw new Error(cleanError || "未知错误");
            }

            const result = new Uint8Array(resultView.length);
            result.set(resultView);

            self.postMessage({
                type: 'decrypted',
                payload: {
                    index: payload.index,
                    result: result.buffer
                }
            }, [result.buffer]);

        } catch (error) {
            self.postMessage({
                type: 'error',
                payload: {
                    index: payload.index,
                    error: error.message
                }
            });
        } finally {
            if (inputDataPtr) {
                wasmModule._free(inputDataPtr);
            }
        }
    }
};
```

