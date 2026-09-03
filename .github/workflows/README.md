# GitHub Actions: Z96A Mali-G52 Kernel Build

## 用途

为 RK3568 Z96A v2 板编译 kernel，解决 Mali-G52 GPU 没驱动问题。

## 使用方法

### 方案 A: Fork 仓库触发(推荐)

1. 在 GitHub 上 fork `kemp233/linux-rockchip`
2. 把这个目录 `.github/workflows/` 推到 fork
3. 在 fork 仓库的 Actions 页面手动触发 workflow
4. 等待 10-30 分钟 (编译时间)
5. 从 Artifacts 下载:
   - `Image` (内核镜像)
   - `rk3568-z96a-laptop-v2.dtb` (dtb)
   - `modules.tar.gz` (内核模块)

### 方案 B: 单独仓库触发

把这个目录做成独立 GitHub repo:

```bash
mkdir z96a-build && cd z96a-build
git init
mkdir -p .github/workflows
cp /tmp/actions-work/.github/workflows/build-kernel-mali-g52.yml .github/workflows/
git add .
git commit -m "init z96a kernel build"
gh repo create z96a-build --public --source=. --push
```

## 关键修改

```yaml
- repository: kemp233/linux-rockchip
  ref: 9c4c29d08bb2b0e0bcb96d223131e64c8aec8b30
```

`9c4c29d08bb2` 是你这板子当前 Image 实际对应的 commit。

## 烧写步骤

下载 artifact 后:

```bash
# 烧 Image 到 boot 分区
mount /dev/mmcblk0p1 /mnt
cp /mnt/Image /mnt/Image.bak
cp Image /mnt/Image/

# 烧 dtb (如果需要)
cp rk3568-z96a-laptop-v2.dtb /mnt/dtb/rockchip/

# 解压 modules
tar xzf modules.tar.gz -C /usr/
sync && reboot
```

## 验证

```bash
dmesg | grep -iE "mali|kbase|fde60000"
ls /sys/bus/platform/devices/ | grep fde60000
ls /dev/dri/
glxinfo 2>&1 | head -10
```
