// SPDX-License-Identifier: MPL-2.0

using System.Globalization;
using System.Text;

namespace MapWinGIS.ActiveXTestHost;

internal static class ShapefileFixture
{
    private const double West = 0;
    private const double East = 4;
    private const double South = 0;
    private const double North = 2;

    private static readonly (double X, double Y)[][] Polygons =
    [
        [(West, South), (1.5, South), (1.5, North), (West, North), (West, South)],
        [(2.5, South), (East, South), (East, North), (2.5, North), (2.5, South)]
    ];

    public static void Write(string path)
    {
        string extension = Path.GetExtension(path);
        if (!extension.Equals(".shp", StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("The fixture path must end in .shp.", nameof(path));
        }

        string basePath = path[..^extension.Length];
        byte[][] records = Polygons.Select(BuildPolygonRecord).ToArray();
        WriteShape(path, records);
        WriteIndex(basePath + ".shx", records);
        WriteAttributes(basePath + ".dbf");
        File.WriteAllText(basePath + ".cpg", "UTF-8");
        File.WriteAllText(basePath + ".prj", ProjectionWkt);
    }

    private static void WriteShape(string path, byte[][] records)
    {
        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream, Encoding.ASCII);
        int fileLength = 50 + records.Sum(record => 4 + record.Length / 2);
        WriteHeader(writer, fileLength, 5);
        for (int index = 0; index < records.Length; index++)
        {
            WriteBigEndian(writer, index + 1);
            WriteBigEndian(writer, records[index].Length / 2);
            writer.Write(records[index]);
        }
    }

    private static void WriteIndex(string path, byte[][] records)
    {
        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream, Encoding.ASCII);
        WriteHeader(writer, 50 + records.Length * 4, 5);
        int offset = 50;
        for (int index = 0; index < records.Length; index++)
        {
            WriteBigEndian(writer, offset);
            WriteBigEndian(writer, records[index].Length / 2);
            offset += 4 + records[index].Length / 2;
        }
    }

    private static void WriteHeader(BinaryWriter writer, int length, int shapeType)
    {
        WriteBigEndian(writer, 9994);
        for (int index = 0; index < 5; index++)
        {
            writer.Write(0);
        }
        WriteBigEndian(writer, length);
        writer.Write(1000);
        writer.Write(shapeType);
        writer.Write(West);
        writer.Write(South);
        writer.Write(East);
        writer.Write(North);
        for (int index = 0; index < 4; index++)
        {
            writer.Write(0.0);
        }
    }

    private static byte[] BuildPolygonRecord((double X, double Y)[] points)
    {
        double minX = points.Min(point => point.X);
        double minY = points.Min(point => point.Y);
        double maxX = points.Max(point => point.X);
        double maxY = points.Max(point => point.Y);
        using var memory = new MemoryStream();
        using var writer = new BinaryWriter(memory, Encoding.ASCII);
        writer.Write(5);
        writer.Write(minX);
        writer.Write(minY);
        writer.Write(maxX);
        writer.Write(maxY);
        writer.Write(1);
        writer.Write(points.Length);
        writer.Write(0);
        foreach ((double x, double y) in points)
        {
            writer.Write(x);
            writer.Write(y);
        }
        return memory.ToArray();
    }

    private static void WriteAttributes(string path)
    {
        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream, Encoding.ASCII);
        writer.Write((byte)0x03);
        writer.Write((byte)26);
        writer.Write((byte)11);
        writer.Write((byte)27);
        writer.Write(Polygons.Length);
        writer.Write((short)(33 + 2 * 32));
        writer.Write((short)21);
        writer.Write(new byte[20]);
        WriteField(writer, "id", 'N', 10, 0);
        WriteField(writer, "name", 'C', 10, 0);
        writer.Write((byte)0x0d);
        for (int index = 0; index < Polygons.Length; index++)
        {
            writer.Write((byte)0x20);
            WritePadded(writer, (index + 1).ToString(CultureInfo.InvariantCulture), 10);
            WritePadded(writer, index == 0 ? "west" : "east", 10);
        }
        writer.Write((byte)0x1a);
    }

    private static void WriteField(
        BinaryWriter writer,
        string name,
        char type,
        byte length,
        byte decimals)
    {
        byte[] fieldName = Encoding.ASCII.GetBytes(name.PadRight(11, '\0'));
        writer.Write(fieldName);
        writer.Write((byte)type);
        writer.Write(0);
        writer.Write(length);
        writer.Write(decimals);
        writer.Write(new byte[14]);
    }

    private static void WritePadded(BinaryWriter writer, string value, int length)
    {
        writer.Write(Encoding.ASCII.GetBytes(value.PadRight(length)));
    }

    private static void WriteBigEndian(BinaryWriter writer, int value)
    {
        writer.Write((byte)(value >> 24));
        writer.Write((byte)(value >> 16));
        writer.Write((byte)(value >> 8));
        writer.Write((byte)value);
    }

    private const string ProjectionWkt =
        "PROJCS[\"Amersfoort / RD New\","
        + "GEOGCS[\"Amersfoort\",DATUM[\"Amersfoort\","
        + "SPHEROID[\"Bessel 1841\",6377397.155,299.1528128,"
        + "AUTHORITY[\"EPSG\",\"7004\"]],AUTHORITY[\"EPSG\",\"6289\"]],"
        + "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
        + "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
        + "AUTHORITY[\"EPSG\",\"4289\"]],"
        + "PROJECTION[\"Oblique_Stereographic\"],"
        + "PARAMETER[\"latitude_of_origin\",52.1561605555556],"
        + "PARAMETER[\"central_meridian\",5.38763888888889],"
        + "PARAMETER[\"scale_factor\",0.9999079],"
        + "PARAMETER[\"false_easting\",155000],"
        + "PARAMETER[\"false_northing\",463000],"
        + "UNIT[\"metre\",1,AUTHORITY[\"EPSG\",\"9001\"]],"
        + "AXIS[\"Easting\",EAST],AXIS[\"Northing\",NORTH],"
        + "AUTHORITY[\"EPSG\",\"28992\"]]";
}
