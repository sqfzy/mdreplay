book,trade -> format -> N 个特征

# 离线
1. 收集10小时okx book, trade数据
2. 用python程序跑，得到 N 个特征
3. 用复刻的cpp版程序跑，得到 N 个特征
4. diff N 个特征

# cpp在线实盘
1. 实时收集okx book, trade数据
2. 每个币至少在一秒内得到过一次 N 个特征
3. 写到共享内存
