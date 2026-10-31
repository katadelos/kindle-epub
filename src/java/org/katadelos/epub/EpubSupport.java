package org.katadelos.epub;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
/** Preserves stock extension handling and adds EPUB. No firmware dependencies. */
public final class EpubSupport {
    private EpubSupport() {
    }
    public static boolean isEpub(String path) {
        return path != null && path.toLowerCase(Locale.ENGLISH).endsWith(".epub");
    }
    public static boolean accept(boolean stock, String path) {
        return stock || isEpub(path);
    }
    public static List extensions(List stock) {
        ArrayList result = new ArrayList(stock);
        if (!result.contains("epub"))
            result.add("epub");
        return result;
    }
    public static String[] extensions(String[] stock) {
        for (int i = 0; i < stock.length; i++)
            if ("epub".equals(stock[i]))
                return stock;
        String[] result = new String[stock.length + 1];
        System.arraycopy(stock, 0, result, 0, stock.length);
        result[stock.length] = "epub";
        return result;
    }
}
