# 未定義分量沒有 reaching definition；保留原暫存器，不對空集合解參照或拆分。
x64arm_hlslcc_patch(UseDefineChains.cpp
  [=[                    AddDefineToList(defs, *(*ul)->psSiblings[j]->defines.begin());]=]
  [=[                    if (!(*ul)->psSiblings[j] || (*ul)->psSiblings[j]->defines.empty())
                        return 0;
                    AddDefineToList(defs, *(*ul)->psSiblings[j]->defines.begin());]=])
x64arm_hlslcc_patch(UseDefineChains.cpp
  [=[                ASSERT((*ul)->psSiblings[j] != NULL);
                ASSERT(!(*ul)->psSiblings[j]->defines.empty());]=]
  [=[                if (!(*ul)->psSiblings[j] || (*ul)->psSiblings[j]->defines.empty())
                    return 0;]=])
