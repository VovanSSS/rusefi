package peak.can;

/** Numeric output buffer ABI used by PCANBasic_JNI.GetValue. */
public final class MutableInteger {
    // The native library looks up this field by name and type.
    public int value;

    public MutableInteger(int value) {
        this.value = value;
    }

    public int getValue() {
        return value;
    }

    public void setValue(int value) {
        this.value = value;
    }
}
