package main

// 配額只限制實際提交量，不預先配置。64 位元程序使用引擎支援的容量，
// 並以宿主實體記憶體的一半為上限；32 位元沿用 2 GiB 位址空間配額。
func processMemoryLimitMiB(architecture string, physical uint64) uint64 {
	maximum := uint64(8192)
	if architecture == "x86" {
		maximum = 2048
	}
	if physical == 0 {
		return 2048
	}
	return max(uint64(64), min(maximum, physical/2/(1024*1024)))
}
