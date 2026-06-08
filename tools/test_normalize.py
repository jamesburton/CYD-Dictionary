from normalize import norm_key
def test_norm():
    assert norm_key("Achilles' heel") == "achilles heel"
    assert norm_key("Mount  Olympus") == "mount olympus"
    assert norm_key("café") == "cafe"
    assert norm_key("ZEUS") == "zeus"
    assert norm_key("well-known") == "well-known"
    print("ok")
test_norm()
