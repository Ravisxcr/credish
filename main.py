from credish import CredishClient
import time

client = CredishClient()
client.set("foo", "bar")
print(type(client.get("foo")))  # <class 'bytes'>

s = time.perf_counter()
client.set("counter", 0)
for i in range(1_000_000):
    client.incrby("counter", 1)
print(client.get("counter"))  # b'10'
e = time.perf_counter()
print(f"Time taken: {e-s:.2f} seconds")


client.set("status", "🚀")

# Getting it back
raw_data = client.get("status") 
print(raw_data)  # Output: b'\xf0\x9f\x9a\x80'

# Making it readable again
print(raw_data.decode("utf-8"))

client.set("🔥", "on fire")
print(client.get("🔥").decode("utf-8"))  # Output: on fire
