// =============================================================
// pubgmhd_match_call.js
// 目的: 在 PUBG Mobile (com.tencent.tmgp.pubgmhd) 进程中,
//       动态 (按函数名查 UFunction::Func) 调用 GameState 上的
//       BlueprintCallable getter 验证对局状态读取.
//
// 用法 (主机侧):
//   frida -U -n com.tencent.tmgp.pubgmhd \
//     -l tools/frida/pubgmhd_match_call.js -o build/reports/match_call.log
//
// 注: 需要游戏进入大厅/对局 ~30s+ 才能保证 GameState/UClass 已就位
// =============================================================

"use strict";

let __diag_dumped = false;
// ---- libUE4.so 内部偏移 (RVA) ----
const RVA_GNAMES = 0x154ce510; // *(void**)
const RVA_GUOBJECTARRAY = 0x14cec650; // 结构体地址 (不解引用)
const RVA_GWORLD = 0x14cee938; // *(void**)

// ---- UE4.18 (PUBG 腾讯版) 结构体偏移 ----
const OFF_OBJ_CLASS = 0x10; // UObject::ClassPrivate
const OFF_OBJ_NAME = 0x18; // UObject::NamePrivate.ComparisonIndex
const OFF_FIELD_NEXT = 0x28; // UField::Next
const OFF_STRUCT_SUPER = 0x30;
const OFF_STRUCT_CHILDREN = 0x38;
const OFF_FUNC_NUMPARMS = 0x8c;
const OFF_FUNC_PARMSSIZE = 0x8e;
const OFF_FUNC_FUNC = 0xb0; // UFunction::Func (Native thunk)

const OFF_WORLD_GS = 0xac0; // World::GameState (dump.cs 验证)

// GUObjectArray (PUBG 自定义分块)
const OFF_GUO_CHUNKS_PTR = 0xc8; // void* chunks[N]   (FUObjectItem* chunks[])
const OFF_GUO_CHUNK_NUM = 0xe8; // int32 numInChunk[N]
const OFF_GUO_NUM_CHUNKS = 0xf8; // int32
const OFF_GUO_TOTAL = 0x100; // int32
const SIZEOF_FUOBJITEM = 0x18;

// TNameEntryArray (chunks of FNameEntry*)
const OFF_NAMES_NUM = 0x1400;
const NAMES_PER_CHUNK = 16384;

// ----------------------------------------------------------------
// libc 直写 (避免 Frida console.log 在大批量时丢日志/卡 V8)
// ----------------------------------------------------------------
const open = new NativeFunction(Module.getGlobalExportByName("open"), "int", [
  "pointer",
  "int",
  "int",
]);
const write = new NativeFunction(Module.getGlobalExportByName("write"), "int", [
  "int",
  "pointer",
  "int",
]);
const close = new NativeFunction(Module.getGlobalExportByName("close"), "int", [
  "int",
]);
const LOG_PATH =
  "/sdcard/Android/data/com.tencent.tmgp.pubgmhd/files/match_call.log";
let g_logFd = -1;
function L(s) {
  const line = s + "\n";
  if (g_logFd < 0) {
    const p = Memory.allocUtf8String(LOG_PATH);
    g_logFd = open(p, 1 | 64 | 512, 0o644); // O_WRONLY|O_CREAT|O_TRUNC
  }
  const buf = Memory.allocUtf8String(line);
  write(g_logFd, buf, line.length);
  console.log(s);
}

// ----------------------------------------------------------------
// 模块基址 (等待 libUE4.so 加载)
// ----------------------------------------------------------------
function waitForUE4(maxWaitMs) {
  const start = Date.now();
  let mod = Process.findModuleByName("libUE4.so");
  while (!mod && Date.now() - start < maxWaitMs) {
    Thread.sleep(0.5);
    mod = Process.findModuleByName("libUE4.so");
  }
  return mod;
}

const ue4 = waitForUE4(60000);
if (!ue4) {
  L("[!] libUE4.so 60s 内未加载, 退出");
  throw new Error("no libUE4.so");
}
L("[*] libUE4.so base = " + ue4.base + "  size=0x" + ue4.size.toString(16));

const G_NAMES_ADDR = ue4.base.add(RVA_GNAMES).readPointer(); // 解引用一次
const G_UOBJECT_ARRAY_PTR = ue4.base.add(RVA_GUOBJECTARRAY).readPointer(); // PUBG: RVA 是指针变量地址, 需解引用得到内部结构体
const G_WORLD_VAR_ADDR = ue4.base.add(RVA_GWORLD); // 还要再解引用读 World*

L("[*] GNames        = " + G_NAMES_ADDR);
L("[*] GUObjectArray = " + G_UOBJECT_ARRAY_PTR);
L("[*] &GWorld       = " + G_WORLD_VAR_ADDR);

// ----------------------------------------------------------------
// FName 解析 (TNameEntryArray)
// ----------------------------------------------------------------
const nameCache = new Map();
function getName(idx) {
  if (idx < 0 || idx > 0x800000) return "<bad:" + idx + ">";
  if (nameCache.has(idx)) return nameCache.get(idx);
  try {
    const chunkIndex = (idx / NAMES_PER_CHUNK) | 0;
    const inChunk = idx % NAMES_PER_CHUNK;
    const chunkPtrAddr = G_NAMES_ADDR.add(chunkIndex * 8);
    const chunkPtr = chunkPtrAddr.readPointer();
    if (chunkPtr.isNull()) return "<null-chunk>";
    const entryPtr = chunkPtr.add(inChunk * 8).readPointer();
    if (entryPtr.isNull()) return "<null-entry>";
    // FNameEntry 校验: bit0 of Index 是 IsWide. AnsiName @ +0x0C
    const ansi = entryPtr.add(0x0c).readUtf8String();
    const s = ansi || "";
    nameCache.set(idx, s);
    return s;
  } catch (e) {
    return "<ex:" + e.message + ">";
  }
}

function readObjName(obj) {
  try {
    const idx = obj.add(OFF_OBJ_NAME).readU32();
    return getName(idx);
  } catch (e) {
    return "<ex>";
  }
}

function readClassName(obj) {
  try {
    const cls = obj.add(OFF_OBJ_CLASS).readPointer();
    if (cls.isNull()) return "<null>";
    return readObjName(cls);
  } catch (e) {
    return "<ex>";
  }
}

function readObjClass(obj) {
  try {
    return obj.add(OFF_OBJ_CLASS).readPointer();
  } catch (e) {
    return ptr(0);
  }
}

// ----------------------------------------------------------------
// GUObjectArray 遍历 (PUBG 分块布局)
// ----------------------------------------------------------------
function enumObjects(callback, quiet) {
  const numChunks = G_UOBJECT_ARRAY_PTR.add(OFF_GUO_NUM_CHUNKS).readS32();
  const total = G_UOBJECT_ARRAY_PTR.add(OFF_GUO_TOTAL).readS32();
  if (!quiet) L("[*] GUObjectArray numChunks=" + numChunks + " total=" + total);
  if (numChunks <= 0 || numChunks > 1000 || total <= 0) return 0;
  let visited = 0;
  for (let ci = 0; ci < numChunks; ci++) {
    const cnt = G_UOBJECT_ARRAY_PTR.add(OFF_GUO_CHUNK_NUM + ci * 4).readS32();
    if (cnt <= 0 || cnt > 65536 * 64) continue;
    const chunkBase = G_UOBJECT_ARRAY_PTR.add(
      OFF_GUO_CHUNKS_PTR + ci * 8,
    ).readPointer();
    if (chunkBase.isNull()) continue;
    if (ci === 0 && !__diag_dumped) {
      __diag_dumped = true;
      L("   [diag] chunk0 base=" + chunkBase + " cnt=" + cnt);
      L(
        "   [diag] chunk0 first 64B = " +
          hexdump(chunkBase, { length: 64, header: false }),
      );
      L(
        "   [diag] item[0] @ " +
          chunkBase +
          " obj* = " +
          chunkBase.readPointer(),
      );
      L(
        "   [diag] item[1] @ " +
          chunkBase.add(0x18) +
          " obj* = " +
          chunkBase.add(0x18).readPointer(),
      );
      L(
        "   [diag] item[2] @ " +
          chunkBase.add(0x30) +
          " obj* = " +
          chunkBase.add(0x30).readPointer(),
      );
    }
    for (let i = 0; i < cnt; i++) {
      const itemAddr = chunkBase.add(i * SIZEOF_FUOBJITEM);
      const objPtr = itemAddr.readPointer();
      if (objPtr.isNull()) continue;
      visited++;
      if (callback(objPtr) === false) return visited;
    }
  }
  return visited;
}

// ----------------------------------------------------------------
// 通过 GUObjectArray 全表扫找当前 GameState 实例
// 判定: ClassPrivate ∈ 我们解析到的 GameState UClass 集合
//        (基类+子类, 涵盖各模式 STExtra/UAE/BR/CarRacing/Lost.../Moba 等)
// ----------------------------------------------------------------
const g_gsClassSet = new Set(); // hex string of UClass addr

function collectGSClasses() {
  g_gsClassSet.clear();
  enumObjects(function (obj) {
    const clsName = readClassName(obj);
    if (clsName !== "Class" && clsName !== "BlueprintGeneratedClass")
      return true;
    const name = readObjName(obj);
    // 只收 GameState 系 (排除 GameMode / GameModeState / 各种 Component)
    if (
      name.indexOf("GameState") >= 0 &&
      name.indexOf("GameModeState") < 0 &&
      name.indexOf("Component") < 0 &&
      name.indexOf("Condition") < 0 &&
      name.indexOf("Listener") < 0 &&
      name.indexOf("Action") < 0 &&
      name.indexOf("AsyncGet") < 0
    ) {
      g_gsClassSet.add(obj.toString());
    }
    return true;
  });
  L("[*] GameState UClass 集合: " + g_gsClassSet.size + " 个");
}

function findCurrentGameStateInstance() {
  if (g_gsClassSet.size === 0) return ptr(0);
  let result = ptr(0);
  let className = "";
  enumObjects(function (obj) {
    const cls = readObjClass(obj);
    if (cls.isNull()) return true;
    if (g_gsClassSet.has(cls.toString())) {
      // 排除 CDO (Default__XYZ): NamePrivate 通常以 Default__ 开头
      const objName = readObjName(obj);
      if (objName && objName.indexOf("Default__") === 0) return true;
      result = obj;
      className = readObjName(cls);
      return false;
    }
    return true;
  }, true);
  if (!result.isNull()) {
    L(
      "   [+] GS instance: " +
        result +
        " name=" +
        readObjName(result) +
        " class=" +
        className,
    );
  }
  return result;
}
const CLASS_CANDIDATES = [
  "STExtraGameStateBase",
  "UAEGameState",
  "GameState",
  "GameStateBase",
];

function findClassByName() {
  let result = null;
  let classCount = 0;
  let stateLikeCount = 0;
  enumObjects(function (obj) {
    const clsName = readClassName(obj);
    // UClass 自己: ClassPrivate 通常名为 "Class" / "BlueprintGeneratedClass"
    if (clsName !== "Class" && clsName !== "BlueprintGeneratedClass")
      return true;
    classCount++;
    const name = readObjName(obj);
    if (
      name &&
      (name.indexOf("GameState") >= 0 || name.indexOf("GameMode") >= 0)
    ) {
      stateLikeCount++;
      L("   [class hit] " + clsName + " / " + name + " @ " + obj);
    }
    for (const cand of CLASS_CANDIDATES) {
      if (name === cand) {
        result = { addr: obj, name: name };
        L("[+] Found UClass " + name + " @ " + obj);
        return true;
      }
    }
    return true;
  });
  L(
    "[*] enum 完成: 见到 UClass 数=" +
      classCount +
      " GameState/Mode 类似名=" +
      stateLikeCount,
  );
  return result;
}

// ----------------------------------------------------------------
// 在 UClass.Children 链 (含 super) 里找 UFunction by name
// ----------------------------------------------------------------
function findFuncInClass(classAddr, funcName) {
  let cur = classAddr;
  while (!cur.isNull()) {
    let child = ptr(0);
    try {
      child = cur.add(OFF_STRUCT_CHILDREN).readPointer();
    } catch (e) {
      break;
    }
    let safety = 0;
    while (!child.isNull() && safety++ < 5000) {
      const childCls = readClassName(child);
      if (childCls === "Function") {
        const cname = readObjName(child);
        if (cname === funcName) {
          return child;
        }
      }
      try {
        child = child.add(OFF_FIELD_NEXT).readPointer();
      } catch (e) {
        break;
      }
    }
    try {
      cur = cur.add(OFF_STRUCT_SUPER).readPointer();
    } catch (e) {
      break;
    }
  }
  return ptr(0);
}

// 兜底: 不依赖 Children 链, 全表扫 UFunction 找名字
function findFuncGlobal(funcName) {
  let result = ptr(0);
  enumObjects(function (obj) {
    if (readClassName(obj) !== "Function") return true;
    if (readObjName(obj) === funcName) {
      result = obj;
      return false;
    }
    return true;
  });
  return result;
}

// ----------------------------------------------------------------
// 调用 BP thunk: void(*)(UObject* Context, FFrame* Stack, void* Result)
// ----------------------------------------------------------------
function callBoolThunk(thunkAddr, contextPtr) {
  if (thunkAddr.isNull() || contextPtr.isNull()) return null;
  const fn = new NativeFunction(thunkAddr, "void", [
    "pointer",
    "pointer",
    "pointer",
  ]);
  const fakeFrame = Memory.alloc(0x100);
  fakeFrame.writeByteArray(new Array(0x100).fill(0));
  const resultBuf = Memory.alloc(8);
  resultBuf.writeU64(0);
  try {
    fn(contextPtr, fakeFrame, resultBuf);
  } catch (e) {
    L("[!] thunk crashed: " + e.message);
    return null;
  }
  return resultBuf.readU8() !== 0;
}

// ----------------------------------------------------------------
// 主流程
// ----------------------------------------------------------------
let g_classInfo = null;
let g_funcInfo = {}; // name -> {addr, parms}
const TARGET_FUNCS = [
  "HasMatchStarted",
  "HasBegunPlay",
  "HasMatchEnded",
  "IsMatchInProgress",
];

function resolveAll() {
  g_classInfo = findClassByName();
  if (!g_classInfo) {
    L("[!] 未找到任何 GameState 类");
    return false;
  }
  collectGSClasses(); // 收集所有 GameState 系 UClass 用于按类型扫实例
  for (const fname of TARGET_FUNCS) {
    let f = findFuncInClass(g_classInfo.addr, fname);
    if (f.isNull()) {
      f = findFuncGlobal(fname);
      if (!f.isNull()) L("[~] " + fname + " 通过全表扫到 (Children 链未命中)");
    }
    if (f.isNull()) {
      L("[!] UFunction " + fname + " 未找到");
      continue;
    }
    const funcPtr = f.add(OFF_FUNC_FUNC).readPointer();
    const numParms = f.add(OFF_FUNC_NUMPARMS).readU8();
    const parmsSize = f.add(OFF_FUNC_PARMSSIZE).readU16();
    const inUE4 =
      funcPtr.compare(ue4.base) >= 0 &&
      funcPtr.compare(ue4.base.add(ue4.size)) < 0;
    L(
      "[+] " +
        fname +
        " UFunction=" +
        f +
        " Func=" +
        funcPtr +
        " (libUE4.so+0x" +
        funcPtr.sub(ue4.base).toString(16) +
        ")" +
        " NumParms=" +
        numParms +
        " ParmsSize=" +
        parmsSize +
        " inUE4=" +
        inUE4,
    );
    g_funcInfo[fname] = {
      addr: funcPtr,
      parms: numParms,
      parmsSize: parmsSize,
      ufunc: f,
    };
  }
  return Object.keys(g_funcInfo).length > 0;
}

function getGameStatePtr() {
  try {
    const world = G_WORLD_VAR_ADDR.readPointer();
    if (world.isNull()) return ptr(0);
    const gs = world.add(OFF_WORLD_GS).readPointer();
    return gs;
  } catch (e) {
    return ptr(0);
  }
}

let tickCount = 0;
function tick() {
  tickCount++;
  const world = G_WORLD_VAR_ADDR.readPointer();
  const worldName = world.isNull() ? "<null>" : readObjName(world);
  L("[t" + tickCount + "] World=" + world + " WorldName=" + worldName);

  if (!g_classInfo) {
    L("[t" + tickCount + "] 解析 GameState UClass + UFunction (一次性 ~5s)...");
    if (!resolveAll()) return;
  }

  // 关键: 不再依赖 World+0xAC0; 直接扫 GUObjectArray 找 GS 实例
  const gs = findCurrentGameStateInstance();
  if (gs.isNull()) {
    L("   [-] 未找到 GameState 实例 -> inMatch=false");
    return;
  }

  for (const fname of Object.keys(g_funcInfo)) {
    const info = g_funcInfo[fname];
    const r = callBoolThunk(info.addr, gs);
    L("  -> " + fname + "(GS) = " + r);
  }
}

setTimeout(function () {
  L("[*] 启动定时探测 (每 3s)");
  tick();
  setInterval(tick, 3000);
}, 1500);
